// SPDX-License-Identifier: Apache-2.0
#include <Arduino.h>
#include <ArduinoJson.h>
#include <M5Unified.hpp>
#include <Preferences.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <vector>

#include "charadock/camera_preview.hpp"
#include "charadock/face_tracking.hpp"
#include "charadock/frame_dispatcher.hpp"
#include "charadock/hardware.hpp"
#include "charadock/hands_free_voice.hpp"
#include "charadock/idle_backlight.hpp"
#include "charadock/mouth_envelope.hpp"
#include "charadock/native_face.hpp"
#include "charadock/portrait_cache.hpp"
#include "charadock/presentation.hpp"
#include "charadock/quick_settings.hpp"
#include "charadock/protocol_v2.hpp"
#include "charadock/wifi_connection.hpp"

#ifndef CHARADOCK_STACKCHAN_FIRMWARE_VERSION
#define CHARADOCK_STACKCHAN_FIRMWARE_VERSION "0.1.0-dev"
#endif

namespace {

constexpr uint32_t kSerialBaud = 500000;
constexpr uint32_t kLongPressMs = 500;
constexpr uint32_t kThinkingPreviewMs = 1800;
constexpr uint32_t kSerialFrameTimeoutMs = 2000;
constexpr size_t kBufferedAudioRingSamples = 262144;
constexpr size_t kFallbackAudioRingSamples = 16384;
constexpr uint32_t kMicSampleRate = 16000;
constexpr size_t kMicBlockSamples = kMicSampleRate / 50;
constexpr uint32_t kLoopRecoveryMagic = 0x43444f43;
constexpr uint32_t kLoopTimeoutMs = 9000;

enum class LoopPhase : uint8_t {
  Input, Transport, Capture, CameraStart, MicrophoneStart, MicrophoneStop,
  SpeakerService, SpeakerStop, Presentation,
};

struct LoopRecovery {
  uint32_t magic;
  uint8_t phase;
};
RTC_NOINIT_ATTR LoopRecovery loopRecovery;
std::atomic<uint32_t> lastLoopAt{0};
std::atomic<LoopPhase> loopPhase{LoopPhase::Input};
int recoveredLoopPhase = -1;

void recoverStalledLoop(void *) {
  if (millis() - lastLoopAt.load() < kLoopTimeoutMs) return;
  loopRecovery.phase = static_cast<uint8_t>(loopPhase.load());
  loopRecovery.magic = kLoopRecoveryMagic;
  esp_restart();
}

void startLoopMonitor() {
  if (loopRecovery.magic == kLoopRecoveryMagic && esp_reset_reason() == ESP_RST_SW)
    recoveredLoopPhase = loopRecovery.phase;
  loopRecovery.magic = 0;
  esp_timer_create_args_t options{};
  options.callback = recoverStalledLoop;
  options.name = "cd-loop";
  esp_timer_handle_t timer;
  lastLoopAt.store(millis());
  if (esp_timer_create(&options, &timer) == ESP_OK)
    esp_timer_start_periodic(timer, 1000000);
}

enum class SerialInputMode : uint8_t {
  Console,
  SawMagicC,
  Frame,
};

charadock::HardwareController hardware;
charadock::IdleBacklight idleBacklight;
charadock::CameraPreview cameraPreview;
bool cameraAllowed = false;
bool privacyMuted = false;
bool privacyTouchHandled = false;
charadock::QuickSettingsTouch quickSettingsTouch;
bool quickSettingsOpen = false;
bool previewFromSettings = false;
uint32_t quickSettingsUsedAt = 0;
bool faceTrackingEnabled = false;
charadock::FaceTracking faceTracking;
uint32_t cameraStartedAt = 0;
uint32_t expressionCueAt = 0;
charadock::Expression expressionCue = charadock::Expression::Neutral;
bool expressionCuePending = false;
bool expressionCueNeedsPause = false;
uint32_t lastVoicedAt = 0;
charadock::DeviceState previousDisplayState = charadock::DeviceState::Connecting;
charadock::PresentationController presentation;
charadock::PortraitCache portraitCache;
charadock::MouthEnvelope mouthEnvelope({360, 1500, 240, 1000, 45, 60});
charadock::PcmPlaybackBuffer audioPlayback;
charadock::PcmPlaybackBuffer stagedPlayback;
bool stageAllowed = false;
uint32_t reportedAudioGeneration = 0;
bool faceWasPresent = false;
int8_t attentionX = 0;
uint32_t lastAttentionStepAt = 0;
std::unique_ptr<charadock::NativeFaceRenderer> face;
std::unique_ptr<charadock::FrameDispatcher> frameDispatcher;
charadock::protocol::Decoder serialFrameDecoder;
std::vector<uint8_t> serialFrameBytes;
String consoleLine;
String deviceId;
uint8_t *portraitSlots[2] = {nullptr, nullptr};
int16_t *audioRingStorage = nullptr;
bool touchWasPressed = false;
bool rawTouchPressed = false;
uint32_t rawTouchChangedAt = 0;
bool pttStarted = false;
bool touchInterruptedPlayback = false;
uint32_t touchStartedAt = 0;
charadock::Expression expressionBeforeTouch = charadock::Expression::Neutral;
bool envelopePreviewActive = false;
uint32_t envelopePreviewStartedAt = 0;
uint32_t envelopePreviewLastBlockAt = 0;
uint32_t serialFrameStartedAt = 0;
size_t serialFrameExpectedBytes = 0;
size_t serialRejectedFrames = 0;
uint32_t lastAudioUnderruns = 0;
uint32_t lastSpeakerGaps = 0;
bool speakerWasQueued = false;
uint32_t lastAudioServiceAt = 0;
uint32_t maxAudioServiceGap = 0;
uint8_t lastVoiceMotion = 0;
uint16_t lastCaptureControlSequence = 0;
uint32_t lastCaptureControlAt = 0;
uint32_t observedSpeakerBlockAt = 0;
size_t observedSpeakerOffset = 0;
uint32_t observedAudioGeneration = 0;
uint32_t observedSpeakerBlock = 0;
uint32_t spokenMouthChanges = 0;
uint32_t gestureRequests = 0;
uint32_t renderedGesturePoses = 0;
uint32_t speechAccents = 0;
charadock::HeadGesturePlayer gestures;

uint8_t lastGesture = 0;
SerialInputMode serialInputMode = SerialInputMode::Console;
charadock::WifiConnection wifi;
charadock::UsbTransport usb(Serial, "usb");
uint16_t eventSequence = 0;
uint32_t usbSeenAt = 0, wifiSeenAt = 0, usbMissingAt = 0;
charadock::protocol::Decoder usbFrameStream;
bool usbReady = false, wifiActive = false, captureActive = false;
bool wasConnected = false, microphoneEnabled = true;
uint8_t usbCaptureMode = 0, wifiCaptureMode = 0;
uint16_t usbVadThreshold = 120, wifiVadThreshold = 120;
bool micMonitoring = false, automaticCapture = false, acceptedCapture = false;
uint32_t lastFaceScanAttemptAt = 0;
bool endDetected = false;
uint32_t micStoppedAt = 0, lastCaptureStatusAt = 0;
uint8_t quietToRearm = 0;
bool waitingForQuiet = false;
uint8_t rearmBlocks = 0;
uint16_t quietRearmThreshold = 0;
charadock::HandsFreeVoice vad;
using MicBlock = std::array<int16_t, kMicBlockSamples>;
std::array<MicBlock, 12> preroll{};
size_t prerollNext = 0, prerollCount = 0;
uint32_t captureStartedAt = 0;
std::array<std::array<int16_t, kMicBlockSamples>, 2> micBlocks = {};
size_t micHead = 0;
size_t micPending = 0;

bool hostConnected() {
  return usbReady || (wifi.hostAuthenticated() && millis() - wifiSeenAt < 24000);
}
charadock::UsbTransport &activeTransport() { return wifiActive ? wifi.transport() : usb; }

void cancelConversationGesture() {
  if (gestures.cancel()) hardware.stopGestureMotion();
  expressionCuePending = false;
  presentation.setFacePose(0, 0);
}

uint8_t captureMode() { return wifiActive ? wifiCaptureMode : usbCaptureMode; }
uint16_t captureThreshold() { return wifiActive ? wifiVadThreshold : usbVadThreshold; }

bool sendMicBlock(const MicBlock &block) {
  return activeTransport().send(charadock::protocol::FrameType::PcmChunk,
                                ++eventSequence,
                                reinterpret_cast<const uint8_t *>(block.data()),
                                block.size() * sizeof(int16_t));
}

void sendCaptureStatus(uint16_t rms, uint32_t now) {
  if (now - lastCaptureStatusAt < 200) return;
  lastCaptureStatusAt = now;
  uint8_t payload[12] = {static_cast<uint8_t>(1 | (captureActive ? 2 : 0))};
  const uint16_t metrics[] = {rms, vad.floor(), vad.startThreshold(captureThreshold()),
                              vad.continueThreshold(captureThreshold())};
  for (size_t i = 0; i < 4; ++i) {
    payload[1 + 2 * i] = metrics[i] & 255;
    payload[2 + 2 * i] = metrics[i] >> 8;
  }
  payload[9] = vad.speech();
  payload[10] = vad.silence();
  payload[11] = vad.calibration();
  activeTransport().send(charadock::protocol::FrameType::CaptureStatus, ++eventSequence,
                         payload, sizeof(payload));
}

bool sendCompletedMicBlocks() {
  // M5Unified reports the number of outstanding requests. They finish in
  // order, so only buffers ahead of the outstanding ones are safe to read.
  while (micPending > M5.Mic.isRecording()) {
    const auto &block = micBlocks[micHead];
    micHead = (micHead + 1) % micBlocks.size();
    --micPending;
    const uint32_t now = millis();
    if (captureActive) presentation.observeMicrophone(block.data(), block.size(), now);
    if (!automaticCapture) {
      if (!sendMicBlock(block)) return false;
      continue;
    }

    const uint16_t rms = charadock::HandsFreeVoice::centeredRms(block.data(), block.size());
    const bool boundary = vad.observe(rms, captureThreshold(), captureActive);
    if (waitingForQuiet) {
      quietToRearm = rms < quietRearmThreshold ? quietToRearm + 1 : 0;
      if (quietToRearm >= 20 || ++rearmBlocks >= 50) {
        waitingForQuiet = false;
        if (quietToRearm >= 20) vad.rearm();
        else vad = {};  // The room noise changed. Measure its new floor.
        prerollCount = prerollNext = 0;
      }
      sendCaptureStatus(rms, now);
      continue;
    }
    if (captureActive) {
      if (!sendMicBlock(block)) return false;
      if (boundary) endDetected = true;
    } else {
      preroll[prerollNext] = block;
      prerollNext = (prerollNext + 1) % preroll.size();
      prerollCount = std::min(prerollCount + 1, preroll.size());
      if (boundary && (captureMode() != 3 || cameraPreview.recentFace(now))) {
        captureActive = true;
        captureStartedAt = now;
        if (!activeTransport().send(charadock::protocol::FrameType::PttStart, ++eventSequence))
          return false;
        const size_t first = (prerollNext + preroll.size() - prerollCount) % preroll.size();
        for (size_t i = 0; i < prerollCount; ++i)
          if (!sendMicBlock(preroll[(first + i) % preroll.size()])) return false;
        prerollCount = 0;
      }
    }
    sendCaptureStatus(rms, now);
    if (endDetected) break;
  }
  return true;
}

void waitForQuietBeforeCapture() {
  waitingForQuiet = true;
  quietToRearm = rearmBlocks = 0;
  quietRearmThreshold = vad.continueThreshold(captureThreshold());
}

void stopCapture(bool complete) {
  const bool wasActive = captureActive;
  const bool submit = wasActive && complete && hostConnected();
  if (submit && !automaticCapture) sendCompletedMicBlocks();
  captureActive = false;
  micMonitoring = false;
  automaticCapture = false;
  endDetected = false;
  micStoppedAt = millis();
  loopPhase.store(LoopPhase::MicrophoneStop);
  M5.Mic.end();
  loopPhase.store(LoopPhase::Capture);
  micHead = 0;
  micPending = 0;
  if (submit) {
    const bool sent = activeTransport().send(charadock::protocol::FrameType::PttEnd, ++eventSequence);
    presentation.setState(sent && acceptedCapture ? charadock::DeviceState::Thinking : charadock::DeviceState::Idle, millis());
  } else if (wasActive) {
    presentation.setState(charadock::DeviceState::Idle, millis());
  }
  acceptedCapture = false;
}
enum class InterruptReason : uint8_t { CaptureFailure = 1, HeadTouch, Transport, CaptureChanged };

void interruptConversation(bool notify = true, InterruptReason reason = InterruptReason::CaptureFailure) {
  stopCapture(false);
  cancelConversationGesture();
  if (privacyMuted || reason == InterruptReason::Transport) cameraPreview.stop();
  else cameraPreview.pauseForConversation();
  presentation.closeSpeech();
  audioPlayback.stop();
  stagedPlayback.stop();
  stageAllowed = false;
  hardware.stopSpeakerStream();
  presentation.setMouthLevel(0);
  presentation.setState(charadock::DeviceState::Idle, millis());
  if (notify && hostConnected()) {
    const uint8_t payload[] = {static_cast<uint8_t>(reason), hardware.headTouchStrength()};
    activeTransport().send(charadock::protocol::FrameType::Interrupt, ++eventSequence, payload,
                           reason == InterruptReason::HeadTouch ? sizeof(payload) : 1);
  }
  if (face) face->setCaption("CharaDock", "");
}
void startCapture() {
  if (privacyMuted || !hostConnected() || !microphoneEnabled) return;
  // Touch-down already interrupted any prior reply. A second asynchronous
  // host Interrupt adjacent to PttStart could cancel this new capture.
  interruptConversation(false);
  if (!M5.Mic.begin()) {
    presentation.setState(charadock::DeviceState::Error, millis());
    return;
  }
  micMonitoring = true;
  acceptedCapture = true;
  captureActive = true;
  captureStartedAt = millis();
  presentation.setState(charadock::DeviceState::Listening, millis());
  const uint8_t manual = 1;
  if (!activeTransport().send(charadock::protocol::FrameType::PttStart, ++eventSequence, &manual, 1))
    stopCapture(false);
}
void updateCapture() {
  const uint32_t now = millis();
  const auto state = presentation.snapshot().state;
  if (privacyMuted) return;
  const bool canListen = state == charadock::DeviceState::Idle || state == charadock::DeviceState::Listening;
  if (micMonitoring && !captureActive && !canListen)
    stopCapture(false);
  const bool cameraCapture = cameraAllowed && captureMode() == 3;
  const bool scanForTracking = cameraAllowed && faceTrackingEnabled && hardware.motionEnabled();
  // Keep one camera session across conversation turns. Paused detection
  // neither consumes frames nor reinitializes the shared I2C bus.
  const bool detectionWanted = hostConnected() && (cameraCapture || scanForTracking);
  const bool pauseDetection = captureActive || state != charadock::DeviceState::Idle ||
      touchWasPressed || audioPlayback.active() || stagedPlayback.active() || hardware.speakerStreamOpen();
  if (cameraPreview.detecting()) {
    if (detectionWanted) cameraPreview.setDetectionPaused(pauseDetection);
    else cameraPreview.stop();
  } else if (detectionWanted && !pauseDetection && !cameraPreview.active() &&
      now - lastFaceScanAttemptAt >= 2500) {
    lastFaceScanAttemptAt = now;
    loopPhase.store(LoopPhase::CameraStart);
    cameraPreview.start(false);
    loopPhase.store(LoopPhase::Capture);
  }
  if (!cameraPreview.active() && !micMonitoring && (captureMode() == 1 || cameraCapture) && hostConnected() &&
      !touchWasPressed && !audioPlayback.active() && !stagedPlayback.active() && !hardware.speakerStreamOpen() &&
      canListen && now - micStoppedAt >= 240) {
    loopPhase.store(LoopPhase::MicrophoneStart);
    if (M5.Mic.begin()) {
      micMonitoring = true;
      automaticCapture = true;
      vad.rearm();
      prerollCount = prerollNext = 0;
    }
    loopPhase.store(LoopPhase::Capture);
  }
  if (!micMonitoring) return;
  if (!hostConnected()) {
    stopCapture(false);
    return;
  }
  if (captureActive && now - captureStartedAt >= (automaticCapture ? 18000u : 29500u)) {
    if (automaticCapture) waitForQuietBeforeCapture();
    stopCapture(true);
    return;
  }
  if (!sendCompletedMicBlocks()) {
    interruptConversation();
    return;
  }
  if (endDetected) {
    stopCapture(true);
    return;
  }
  // Keep a second 20 ms request queued so rendering and USB writes don't
  // leave the I2S reader waiting for each new recording request.
  while (micPending < micBlocks.size()) {
    auto &block = micBlocks[(micHead + micPending) % micBlocks.size()];
    if (!M5.Mic.record(block.data(), block.size(), kMicSampleRate)) {
      interruptConversation();
      return;
    }
    ++micPending;
  }
}

bool canStartCameraPreview() {
  return !privacyMuted && cameraAllowed && !captureActive && !audioPlayback.active() &&
      !hardware.speakerPlaying() && presentation.snapshot().state == charadock::DeviceState::Idle;
}

bool startCameraPreview() {
  if (!canStartCameraPreview()) return false;
  if (micMonitoring) stopCapture(false);
  if (!cameraPreview.start()) return false;
  cameraStartedAt = millis();
  return true;
}

void sendHello(charadock::UsbTransport &target, uint16_t sequence,
               charadock::protocol::FrameType type) {
  JsonDocument doc;
  doc["protocol"] = 2;
  doc["board"] = "m5stack-stackchan-k151";
  doc["deviceId"] = deviceId;
  doc["firmware"] = CHARADOCK_STACKCHAN_FIRMWARE_VERSION;
  if (recoveredLoopPhase >= 0)
    doc["capabilities"]["runtime"]["recoveredLoopPhase"] = recoveredLoopPhase;
  doc["capabilities"]["audio"]["capture"] = true;
  doc["capabilities"]["audio"]["handsFree"] = true;
  doc["capabilities"]["audio"]["captureAbort"] = true;
  doc["capabilities"]["audio"]["captureFinalize"] = true;
  doc["capabilities"]["audio"]["captureControlFrame"] = 0x69;
  doc["capabilities"]["audio"]["maxPrebufferMs"] = charadock::PcmPlaybackBuffer::kMaximumPrebufferMs;
  doc["capabilities"]["audio"]["playbackDiagnostics"] = true;
  doc["capabilities"]["audio"]["preserveVoiceMotionOnReset"] = true;
  doc["capabilities"]["motion"]["voiceCommands"] = true;
  doc["capabilities"]["motion"]["voiceMotionMaxCommand"] = charadock::kLastVoiceMotion;
  doc["capabilities"]["motion"]["readback"] = true;
  doc["capabilities"]["camera"]["conversationGate"] = true;
  doc["capabilities"]["camera"]["localPreview"] = true;
  doc["capabilities"]["camera"]["controls"] = true;
  doc["capabilities"]["transport"]["usbRelease"] = true;
  doc["capabilities"]["privacy"]["muted"] = privacyMuted;
  doc["capabilities"]["motion"]["expressionCues"] = true;
  doc["capabilities"]["motion"]["phraseCues"] = true;
  doc["capabilities"]["motion"]["waitingLook"] = true;
  doc["capabilities"]["motion"]["homePitch"] = true;
  doc["capabilities"]["audio"]["playback"] = audioPlayback.storageReady();
  doc["capabilities"]["audio"]["playbackStatus"] = true;
  doc["capabilities"]["audio"]["imaAdpcm"] = true;
  doc["capabilities"]["audio"]["preferPcm16ForBufferedSpeech"] = true;
  doc["capabilities"]["audio"]["maxAdpcmPcmBytes"] =
      charadock::FrameDispatcher::kMaxAdpcmPcmBytes;
  if (stagedPlayback.storageReady())
    doc["capabilities"]["audio"]["stagedPlaybackSamples"] = stagedPlayback.capacitySamples();
  doc["capabilities"]["audio"]["completePlaybackSamples"] =
      audioPlayback.capacitySamples();
  doc["capabilities"]["audio"]["sampleRate"] = 16000;
  doc["capabilities"]["audio"]["duplex"] = "half";
  doc["capabilities"]["display"]["width"] = 320;
  doc["capabilities"]["display"]["height"] = 240;
  doc["capabilities"]["display"]["bitsPerPixel"] = 16;
  doc["capabilities"]["display"]["bitmap"].add("rgb565le");
  doc["capabilities"]["motion"]["enabled"] = hardware.motionEnabled();
  doc["capabilities"]["motion"]["conversationGestures"] = true;
  String payload;
  serializeJson(doc, payload);
  target.send(type, sequence, reinterpret_cast<const uint8_t *>(payload.c_str()), payload.length());
}

uint32_t parseColor(const String &value, uint32_t fallback) {
  String normalized = value;
  normalized.trim();
  if (normalized.startsWith("#"))
    normalized.remove(0, 1);
  if (normalized.length() != 6)
    return fallback;
  char *end = nullptr;
  const uint32_t parsed = strtoul(normalized.c_str(), &end, 16);
  return end && *end == '\0' ? parsed : fallback;
}

void printCapabilities() {
  JsonDocument doc;
  doc["protocol"] = 2;
  doc["deviceId"] = deviceId;
  doc["model"] = "m5stack-stackchan-k151";
  doc["firmware"] = CHARADOCK_STACKCHAN_FIRMWARE_VERSION;
  JsonObject display = doc["capabilities"]["display"].to<JsonObject>();
  display["width"] = 320;
  display["height"] = 240;
  display["formats"].add("rgb565le");
  display["staticPortrait"] = portraitCache.storageReady();
  display["portraitCacheBytes"] = portraitCache.storageReady()
                                      ? charadock::PortraitCache::kByteCount * 2
                                      : 0;
  JsonObject audio = doc["capabilities"]["audio"].to<JsonObject>();
  audio["capture"] = true;
  audio["playback"] = true;
  audio["duplex"] = "half";
  audio["wireSampleRate"] = 16000;
  audio["usbPcmPlayback"] = audioPlayback.storageReady();
  audio["ringBufferMs"] = audioPlayback.storageReady()
                              ? (audioPlayback.capacitySamples() * 1000) / 16000
                              : 0;
  audio["defaultPrebufferMs"] = 200;
  doc["capabilities"]["motion"].add("yaw");
  doc["capabilities"]["motion"].add("pitch");
  doc["capabilities"]["motion"].add("nod");
  doc["capabilities"]["motion"].add("tilt");
  doc["capabilities"]["touch"] = true;
  doc["capabilities"]["led"] = true;
  doc["capabilities"]["psram"] = ESP.getPsramSize();
  doc["capabilities"]["storage"]["sdOptional"] = true;
  doc["presentation"]["modes"].add("hybrid");
  doc["presentation"]["modes"].add("character-art");
  doc["presentation"]["modes"].add("native-face");
  serializeJson(doc, Serial);
  Serial.println();
}

bool setStateByName(String value) {
  value.trim();
  const uint32_t now = millis();
  if (value == "idle")
    presentation.setState(charadock::DeviceState::Idle, now);
  else if (value == "listening")
    presentation.setState(charadock::DeviceState::Listening, now);
  else if (value == "thinking")
    presentation.setState(charadock::DeviceState::Thinking, now);
  else if (value == "speaking")
    presentation.setState(charadock::DeviceState::Speaking, now);
  else if (value == "error")
    presentation.setState(charadock::DeviceState::Error, now);
  else if (value == "connecting")
    presentation.setState(charadock::DeviceState::Connecting, now);
  else
    return false;
  return true;
}

bool setExpressionByName(String value) {
  value.trim();
  if (value == "neutral")
    presentation.setExpression(charadock::Expression::Neutral);
  else if (value == "listening")
    presentation.setExpression(charadock::Expression::Listening);
  else if (value == "thinking")
    presentation.setExpression(charadock::Expression::Thinking);
  else if (value == "happy")
    presentation.setExpression(charadock::Expression::Happy);
  else if (value == "surprised")
    presentation.setExpression(charadock::Expression::Surprised);
  else if (value == "soft")
    presentation.setExpression(charadock::Expression::Soft);
  else
    return false;
  return true;
}

void printStatus() {
  const auto &state = presentation.snapshot();
  const auto *portrait = portraitCache.metadata();
  Serial.printf("state=%s expression=%s mode=%s mouth=%u motion=%s psram=%u "
                "battery=%.3fV current=%.3fA portrait=%s revision=%s "
                "staged=%u envelope-rms=%u audio=%s buffered=%u/%u "
                "underruns=%u protocol-rejected=%u mouth-changes=%u gestures=%u last-gesture=%u "
                "face-poses=%u accents=%u aperture=%u backlight=%u camera=%u camera-frames=%lu mic-monitor=%u face-scan=%u face-recent=%u face-inferences=%lu next-audio=%u/%u gaze=%d voice-led=%06lx camera-allowed=%u follow=%u target=%d/%d privacy=%u\n",
                charadock::deviceStateName(state.state),
                charadock::expressionName(state.expression),
                charadock::displayModeName(state.displayMode), state.mouthLevel,
                hardware.motionEnabled() ? "on" : "off", ESP.getFreePsram(),
                hardware.batteryVoltage(), hardware.batteryCurrent(),
                portraitCache.available() ? "ready" : "empty",
                portrait ? portrait->revision.data() : "-",
                static_cast<unsigned>(portraitCache.receivedBytes()),
                mouthEnvelope.lastRms(),
                charadock::audioPlaybackPhaseName(audioPlayback.phase()),
                static_cast<unsigned>(audioPlayback.bufferedSamples()),
                static_cast<unsigned>(audioPlayback.capacitySamples()),
                static_cast<unsigned>(audioPlayback.underrunCount()),
                static_cast<unsigned>(serialRejectedFrames),
                static_cast<unsigned>(spokenMouthChanges),
                static_cast<unsigned>(gestureRequests), lastGesture,
                static_cast<unsigned>(renderedGesturePoses),
                static_cast<unsigned>(speechAccents), state.mouthAperture,
                hardware.backlight(), cameraPreview.active(),
                static_cast<unsigned long>(cameraPreview.frameCount()), micMonitoring,
                cameraPreview.detecting(), cameraPreview.recentFace(millis()),
                static_cast<unsigned long>(cameraPreview.inferenceCount()),
                static_cast<unsigned>(stagedPlayback.bufferedSamples()),
                static_cast<unsigned>(stagedPlayback.capacitySamples()), static_cast<int>(attentionX),
                static_cast<unsigned long>(hardware.voiceLedRgb()), cameraAllowed, faceTrackingEnabled,
                faceTracking.yaw(), faceTracking.pitch(), privacyMuted);
}

uint16_t rgb565(uint8_t red, uint8_t green, uint8_t blue) {
  return static_cast<uint16_t>(((red & 0xf8) << 8) | ((green & 0xfc) << 3) |
                               (blue >> 3));
}

void fillTestPortraitRow(uint16_t *row, uint16_t y) {
  for (uint16_t x = 0; x < charadock::PortraitCache::kWidth; ++x) {
    const uint8_t warmth = static_cast<uint8_t>((x * 20) / 319);
    row[x] = rgb565(static_cast<uint8_t>(244 + warmth / 2),
                    static_cast<uint8_t>(232 + warmth / 3),
                    static_cast<uint8_t>(207 - warmth / 2));
    const int32_t leftEye =
        (static_cast<int32_t>(x) - 105) * (static_cast<int32_t>(x) - 105) +
        (static_cast<int32_t>(y) - 104) * (static_cast<int32_t>(y) - 104);
    const int32_t rightEye =
        (static_cast<int32_t>(x) - 215) * (static_cast<int32_t>(x) - 215) +
        (static_cast<int32_t>(y) - 104) * (static_cast<int32_t>(y) - 104);
    if (leftEye < 17 * 17 || rightEye < 17 * 17)
      row[x] = rgb565(23, 21, 19);
    const int32_t mouthX = static_cast<int32_t>(x) - 160;
    const int32_t mouthY = static_cast<int32_t>(y) - 163;
    if ((mouthX * mouthX) / (29 * 29) + (mouthY * mouthY) / (8 * 8) < 1) {
      row[x] = rgb565(217, 154, 36);
    }
  }
}

bool installTestPortrait() {
  if (!portraitCache.storageReady())
    return false;
  uint16_t row[charadock::PortraitCache::kWidth];
  uint32_t checksum = 0xffffffffu;
  for (uint16_t y = 0; y < charadock::PortraitCache::kHeight; ++y) {
    fillTestPortraitRow(row, y);
    checksum = charadock::protocol::crc32(
        reinterpret_cast<const uint8_t *>(row), sizeof(row), checksum);
  }
  charadock::PortraitMetadata metadata;
  metadata.width = charadock::PortraitCache::kWidth;
  metadata.height = charadock::PortraitCache::kHeight;
  metadata.byteCount = charadock::PortraitCache::kByteCount;
  metadata.checksum = checksum ^ 0xffffffffu;
  snprintf(metadata.revision.data(), metadata.revision.size(),
           "bringup-test-v1");
  snprintf(metadata.frameName.data(), metadata.frameName.size(), "portrait");
  auto result = portraitCache.beginTransfer(metadata);
  for (uint16_t y = 0; result == charadock::PortraitCacheResult::Ok &&
                       y < charadock::PortraitCache::kHeight;
       ++y) {
    fillTestPortraitRow(row, y);
    result = portraitCache.writeChunk(static_cast<uint32_t>(y) * sizeof(row),
                                      reinterpret_cast<const uint8_t *>(row),
                                      sizeof(row));
  }
  if (result == charadock::PortraitCacheResult::Ok)
    result = portraitCache.finishTransfer();
  if (result != charadock::PortraitCacheResult::Ok) {
    portraitCache.abortTransfer();
    Serial.printf("portrait test failed: %s\n",
                  charadock::portraitCacheResultName(result));
    return false;
  }
  presentation.setPortraitAvailable(true);
  presentation.requestPortrait(millis());
  return true;
}

void initializePortraitCache() {
  for (auto &slot : portraitSlots) {
    slot = static_cast<uint8_t *>(
        heap_caps_malloc(charadock::PortraitCache::kByteCount,
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  if (!portraitSlots[0] || !portraitSlots[1] ||
      !portraitCache.attach(portraitSlots[0], portraitSlots[1],
                            charadock::PortraitCache::kByteCount)) {
    for (auto &slot : portraitSlots) {
      if (slot)
        heap_caps_free(slot);
      slot = nullptr;
    }
    Serial.println("warning: portrait PSRAM cache unavailable; Native Face "
                   "fallback remains active");
  }
  presentation.setPortraitAvailable(portraitCache.available());
}

void initializeAudioPlayback() {
  size_t capacity = kBufferedAudioRingSamples;
  audioRingStorage = static_cast<int16_t *>(
      heap_caps_malloc(capacity * sizeof(int16_t),
                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!audioRingStorage) {
    capacity = kFallbackAudioRingSamples;
    audioRingStorage = static_cast<int16_t *>(
        heap_caps_malloc(capacity * sizeof(int16_t), MALLOC_CAP_8BIT));
  }
  if (!audioPlayback.attach(audioRingStorage, capacity)) {
    if (audioRingStorage)
      heap_caps_free(audioRingStorage);
    audioRingStorage = nullptr;
    Serial.println("warning: PCM playback ring unavailable");
  }
  if (audioRingStorage && capacity == kBufferedAudioRingSamples) {
    auto *stagedStorage = static_cast<int16_t *>(heap_caps_malloc(
        capacity * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (stagedStorage) stagedPlayback.attach(stagedStorage, capacity);
  }
  observedAudioGeneration = audioPlayback.generation();
}

void handleConsoleCommand(String command) {
  command.trim();
  if (!command.length())
    return;
  const int separator = command.indexOf(' ');
  const String name = separator < 0 ? command : command.substring(0, separator);
  String value = separator < 0 ? "" : command.substring(separator + 1);
  value.trim();
  bool ok = true;
  if (name == "help") {
    Serial.println("commands: capabilities | status | state <name> | "
                   "expression <name> | mouth <0..2>");
    Serial.println("          mode <hybrid|character-art|native-face> | theme "
                   "<primary> <secondary> <accent>");
    Serial.println("          artwork <on|off> | portrait "
                   "<test|show|invalidate>");
    Serial.println("          motion <on|off|test> | audio "
                   "<speaker-test|mic-test|envelope-preview|stream-stop>");
    Serial.println("          display <snapshot|settings|face> | camera <preview|stop> (USB diagnostic)");
  } else if (name == "capabilities") {
    printCapabilities();
  } else if (name == "status") {
    printStatus();
  } else if (name == "display" && (value == "settings" || value == "face") && !usbReady) {
    quickSettingsOpen = value == "settings";
    quickSettingsUsedAt = millis();
    idleBacklight.noteActivity(quickSettingsUsedAt);
  } else if (name == "display" && value == "snapshot") {
    if (!usbReady && !cameraPreview.active() && !captureActive && !audioPlayback.active() &&
        !hardware.speakerPlaying() && face)
      ok = face->writeSnapshot(Serial);
    else
      ok = false;
  } else if (name == "camera") {
    if (value == "preview") ok = startCameraPreview();
    else if (value == "stop") cameraPreview.stop();
    else ok = false;
  } else if (name == "state") {
    ok = setStateByName(value);
  } else if (name == "expression") {
    ok = setExpressionByName(value);
  } else if (name == "mouth") {
    presentation.setMouthLevel(static_cast<uint8_t>(value.toInt()));
  } else if (name == "mode") {
    if (value == "hybrid") {
      presentation.setDisplayMode(charadock::DisplayMode::Hybrid);
      presentation.requestPortrait(millis());
    } else if (value == "character-art") {
      presentation.setDisplayMode(charadock::DisplayMode::CharacterArt);
      presentation.requestPortrait(millis());
    } else if (value == "native-face") {
      presentation.setDisplayMode(charadock::DisplayMode::NativeFace);
    } else {
      ok = false;
    }
  } else if (name == "theme") {
    const int first = value.indexOf(' ');
    const int second = first < 0 ? -1 : value.indexOf(' ', first + 1);
    if (first < 0 || second < 0) {
      ok = false;
    } else {
      charadock::Theme theme = presentation.snapshot().theme;
      theme.primary = parseColor(value.substring(0, first), theme.primary);
      theme.secondary =
          parseColor(value.substring(first + 1, second), theme.secondary);
      theme.accent = parseColor(value.substring(second + 1), theme.accent);
      presentation.setTheme(theme);
    }
  } else if (name == "artwork") {
    if (value == "on")
      presentation.setArtworkAllowed(true);
    else if (value == "off") {
      presentation.setArtworkAllowed(false);
      portraitCache.invalidate();
      presentation.setPortraitAvailable(false);
    } else
      ok = false;
  } else if (name == "portrait") {
    if (value == "test")
      ok = installTestPortrait();
    else if (value == "show") {
      presentation.requestPortrait(millis());
      ok = portraitCache.available();
    } else if (value == "invalidate") {
      portraitCache.invalidate();
      presentation.setPortraitAvailable(false);
    } else
      ok = false;
  } else if (name == "motion") {
    if (value == "on")
      hardware.setMotionEnabled(true);
    else if (value == "off")
      hardware.setMotionEnabled(false);
    else if (value == "test")
      hardware.runConservativeMotionTest();
    else if (value == "status")
      Serial.printf("motion=%s home-pitch=%u actual-pitch=%d\n",
                    hardware.motionEnabled() ? "on" : "off", hardware.homePitch(),
                    hardware.motionEnabled() ? hardware.currentPitch() : -1);
    else
      ok = false;
  } else if (name == "audio") {
    stagedPlayback.stop();
    stageAllowed = false;
    if (value == "speaker-test" && !privacyMuted) {
      audioPlayback.stop();
      hardware.stopSpeakerStream();
      hardware.runSpeakerTest();
    } else if (value == "mic-test" && !privacyMuted) {
      audioPlayback.stop();
      hardware.stopSpeakerStream();
      Serial.printf("microphone-rms=%u\n", hardware.sampleMicrophoneRms());
    } else if (value == "envelope-preview") {
      audioPlayback.stop();
      hardware.stopSpeakerStream();
      mouthEnvelope.reset(millis());
      envelopePreviewStartedAt = millis();
      envelopePreviewLastBlockAt = 0;
      envelopePreviewActive = true;
      presentation.setState(charadock::DeviceState::Speaking, millis());
      presentation.setMouthLevel(0);
    } else if (value == "stream-stop") {
      audioPlayback.stop();
      hardware.stopSpeakerStream();
      presentation.setMouthLevel(0);
      presentation.setState(charadock::DeviceState::Idle, millis());
    } else
      ok = false;
  } else {
    ok = false;
  }
  if (!ok)
    Serial.printf("error: unsupported command: %s\n", command.c_str());
  else if (name != "status" && name != "capabilities" && name != "display" && name != "help")
    idleBacklight.noteActivity(millis());
}

void consumeConsoleByte(uint8_t value) {
  if (value == '\r')
    return;
  if (value == '\n') {
    handleConsoleCommand(consoleLine);
    consoleLine = "";
  } else if (value >= 0x20 && value <= 0x7e && consoleLine.length() < 180) {
    consoleLine += static_cast<char>(value);
  }
}

void resetSerialFrameInput() {
  serialFrameBytes.clear();
  serialFrameExpectedBytes = 0;
  serialInputMode = SerialInputMode::Console;
}

void sendProtocolOutcome(const charadock::protocol::Frame &request,
                         const charadock::FrameApplyOutcome &outcome,
                         charadock::UsbTransport *target = nullptr) {
  const bool accepted =
      outcome.result == charadock::FrameApplyResult::Applied ||
      outcome.result == charadock::FrameApplyResult::PortraitCompleted ||
      outcome.result == charadock::FrameApplyResult::PortraitCacheHit;
  charadock::protocol::Frame response;
  response.type = accepted ? charadock::protocol::FrameType::Ack
                           : charadock::protocol::FrameType::Error;
  response.sequence = request.sequence;
  response.payload = {static_cast<uint8_t>(request.type),
                      static_cast<uint8_t>(outcome.result),
                      static_cast<uint8_t>(outcome.portraitResult),
                      static_cast<uint8_t>(outcome.audioResult)};
  (target ? *target : usb).send(response.type, response.sequence,
                               response.payload.data(), response.payload.size());
}

void handleConnectedFrame(const charadock::protocol::Frame &frame, bool network) {
  using charadock::protocol::FrameType;
  auto &target = network ? wifi.transport() : usb;
  const auto reply = [&](bool ok) {
    sendProtocolOutcome(frame, {ok ? charadock::FrameApplyResult::Applied
                                   : charadock::FrameApplyResult::InvalidPayload}, &target);
  };
  if (network && frame.type == FrameType::AuthChallenge) {
    uint8_t proof[32];
    if (wifi.createAuthenticationProof(frame.payload.data(), frame.payload.size(), proof))
      target.send(FrameType::DeviceAuth, frame.sequence, proof, sizeof(proof));
    return;
  }
  if (frame.type == FrameType::HostHello) {
    const bool authenticated = wifi.hostAuthenticated();
    if (network && ((!authenticated &&
        !wifi.markHostAuthenticated(frame.payload.data(), frame.payload.size())) ||
        (authenticated && !frame.payload.empty()))) {
      wifi.resetHost(); return;
    }
    if (network) wifiSeenAt = millis();
    else { usbReady = true; usbSeenAt = millis(); }
    reply(true);
    sendHello(target, frame.sequence, FrameType::DeviceHello);
    return;
  }
  if (network && !wifi.hostAuthenticated()) { wifi.resetHost(); return; }
  if (network) wifiSeenAt = millis();
  else if (usbReady) { usbSeenAt = millis(); usbMissingAt = 0; }
  if (frame.type == FrameType::DeviceHello || frame.type == FrameType::Capabilities) {
    sendHello(target, frame.sequence, frame.type); return;
  }
  if (frame.type == FrameType::WifiConfig) {
    const char *error = "";
    reply(!network && wifi.provision(frame.payload.data(), frame.payload.size(), error));
    return;
  }
  if (frame.type == FrameType::WifiStatus) {
    wifi.sendStatus(target, frame.sequence, wifi.networkConnected() ? "connected" : "disconnected");
    return;
  }
  if (frame.type == FrameType::CaptureConfig) {
    // PTT=0, hands-free=1, disabled=2, camera-assisted hands-free=3.
    if (frame.payload.size() != 3 || frame.payload[0] > 3) {
      reply(false); return;
    }
    const uint8_t oldMode = captureMode();
    (network ? wifiCaptureMode : usbCaptureMode) = frame.payload[0];
    const uint16_t threshold = frame.payload[1] | (frame.payload[2] << 8);
    (network ? wifiVadThreshold : usbVadThreshold) = std::clamp<uint16_t>(threshold, 80, 800);
    microphoneEnabled = captureMode() != 2;
    if (oldMode != captureMode()) {
      waitingForQuiet = false;
      quietToRearm = 0;
      if (cameraPreview.detecting()) cameraPreview.stop();
    }
    if (oldMode != captureMode() && micMonitoring) {
      if (captureActive) interruptConversation(true, InterruptReason::CaptureChanged);
      else stopCapture(false);
    }
    reply(true); return;
  }
  if (static_cast<uint8_t>(frame.type) == 0x6A) {
    if (network || !usbReady || !wifi.configured() || !frame.payload.empty()) {
      reply(false);
      return;
    }
    // Closing a desktop serial handle can leave CDC present while USB still
    // supplies power. Release the lease before the host closes that handle.
    reply(true);
    usbReady = false;
    usbMissingAt = 0;
    usbFrameStream.reset();
    return;
  }
  // Allow diagnostics and standby configuration on both links, but only
  // the USB-preferred owner may change conversation/display state.
  if (network && usbReady) { reply(false); return; }
  if (static_cast<uint8_t>(frame.type) == 0x69 ||
      static_cast<uint8_t>(frame.type) == 0x34) {
    if (frame.payload.size() > 1 || (!frame.payload.empty() && frame.payload[0] != 1)) {
      reply(false);
      return;
    }
    if (static_cast<uint8_t>(frame.type) == 0x69) {
      const uint32_t now = millis();
      if (lastCaptureControlAt && now - lastCaptureControlAt < 2000 &&
          frame.sequence == lastCaptureControlSequence) {
        reply(true);
        return;
      }
      lastCaptureControlSequence = frame.sequence;
      lastCaptureControlAt = now;
    }
    if (captureActive && automaticCapture) waitForQuietBeforeCapture();
    if (micMonitoring || captureActive) stopCapture(!frame.payload.empty());
    reply(true);
    return;
  }
  if (static_cast<uint8_t>(frame.type) == 0x65) {
    const uint8_t data[] = {static_cast<uint8_t>(std::min<uint32_t>(255, lastAudioUnderruns)),
                            static_cast<uint8_t>(std::min<uint32_t>(255, lastSpeakerGaps)),
                            static_cast<uint8_t>(std::min<uint32_t>(255, maxAudioServiceGap))};
    target.send(frame.type, frame.sequence, data, sizeof(data));
    return;
  }
  if (static_cast<uint8_t>(frame.type) == 0x66) {
    if (frame.payload.size() != 1 || frame.payload[0] < charadock::kFirstVoiceMotion ||
        frame.payload[0] > charadock::kLastVoiceMotion ||
        !hardware.motionEnabled() || captureActive || hardware.headTouched()) {
      reply(false);
      return;
    }
    const bool requested = gestures.request(static_cast<charadock::HeadGesture>(frame.payload[0]), millis());
    if (requested) lastVoiceMotion = frame.payload[0];
    reply(requested);
    return;
  }
  if (static_cast<uint8_t>(frame.type) == 0x68) {
    const bool on = hardware.motionEnabled();
    const auto yaw = static_cast<uint16_t>(on ? hardware.currentYaw() : 0);
    const auto pitch = static_cast<uint16_t>(on ? hardware.currentPitch() : 0);
    const uint8_t data[] = {static_cast<uint8_t>(on), lastVoiceMotion,
                            static_cast<uint8_t>(yaw), static_cast<uint8_t>(yaw >> 8),
                            static_cast<uint8_t>(pitch), static_cast<uint8_t>(pitch >> 8),
                            static_cast<uint8_t>(renderedGesturePoses), static_cast<uint8_t>(speechAccents)};
    target.send(frame.type, frame.sequence, data, sizeof(data));
    return;
  }
  if (frame.type == FrameType::AudioStatus) {
    if (!frame.payload.empty()) { reply(false); return; }
    const uint8_t status = audioPlayback.active() || hardware.speakerStreamOpen();
    target.send(frame.type, frame.sequence, &status, 1);
    return;
  }
  if (frame.type == FrameType::Motion) {
    if (!hostConnected() || frame.payload.size() != 1 || frame.payload[0] > 1) { reply(false); return; }
    hardware.setMotionEnabled(frame.payload[0] == 1);
    reply(true); return;
  }
  if (frame.type == FrameType::Gesture) {
    using charadock::HeadGesture;
    if (!hostConnected() || frame.payload.size() != 1 ||
        frame.payload[0] < static_cast<uint8_t>(HeadGesture::Waiting) ||
        (frame.payload[0] > static_cast<uint8_t>(HeadGesture::Reassure) &&
         frame.payload[0] != static_cast<uint8_t>(HeadGesture::WaitingLook))) {
      reply(false); return;
    }
    if (!captureActive && !hardware.headTouched()) {
      ++gestureRequests;
      lastGesture = frame.payload[0];
      gestures.request(static_cast<HeadGesture>(frame.payload[0]), millis());
    }
    reply(true); return;
  }
  if (static_cast<uint8_t>(frame.type) == 0x61) {
    const bool started = frame.payload.empty() && startCameraPreview();
    if (started) {
      previewFromSettings = false;
      quickSettingsOpen = false;
    }
    reply(started); return;
  }
  if (static_cast<uint8_t>(frame.type) == 0x64) {
    if (frame.payload.size() != 1 || frame.payload[0] > 3) { reply(false); return; }
    cameraAllowed = frame.payload[0] & 1;
    faceTrackingEnabled = cameraAllowed && (frame.payload[0] & 2);
    if (!cameraAllowed) cameraPreview.stop();
    reply(true);
    return;
  }
  if (static_cast<uint8_t>(frame.type) == 0x63) {
    if (frame.payload.size() != 1 || frame.payload[0] < 5 || frame.payload[0] > 20) {
      reply(false);
      return;
    }
    hardware.setHomePitch(frame.payload[0], !captureActive && !hardware.speakerPlaying() && !hardware.headTouched());
    reply(true);
    return;
  }
  if (static_cast<uint8_t>(frame.type) == 0x62) {
    if (frame.payload.empty() || frame.payload.size() > 2 ||
        (frame.payload.size() == 2 && frame.payload[1] != 1) ||
        frame.payload[0] > static_cast<uint8_t>(charadock::Expression::Soft)) {
      reply(false); return;
    }
    expressionCue = static_cast<charadock::Expression>(frame.payload[0]);
    expressionCueAt = millis();
    expressionCuePending = true;
    expressionCueNeedsPause = frame.payload.size() == 2;
    reply(true); return;
  }
  if (static_cast<uint8_t>(frame.type) == 0x60) {
    JsonDocument scene;
    if (frame.payload.size() > 1024 || deserializeJson(scene, frame.payload.data(), frame.payload.size()) ||
        !scene["caption"].is<const char *>() || !scene["name"].is<const char *>()) { reply(false); return; }
    if (face && face->setCaption(scene["name"].as<const char *>(), scene["caption"].as<const char *>()))
      idleBacklight.noteActivity(millis());
    reply(true); return;
  }
  if (privacyMuted && (frame.type == FrameType::AudioBegin || frame.type == FrameType::AudioChunk ||
      frame.type == FrameType::AudioEnd ||
      (frame.type >= FrameType::AudioStageBegin && frame.type <= FrameType::AudioStagePlay))) {
    reply(false);
    return;
  }
  if (frame.type >= FrameType::AudioStageBegin && frame.type <= FrameType::AudioStagePlay && !stageAllowed) {
    reply(false);
    return;
  }
  if (frame.type == FrameType::AudioStagePlay) {
    if (!frame.payload.empty() || captureActive || hardware.headTouched() ||
        hardware.speakerStreamOpen() || !audioPlayback.adoptStaged(stagedPlayback)) {
      reply(false);
      return;
    }
    cameraPreview.pauseForConversation();
    stopCapture(false);
    reply(true);
    return;
  }
  if (frame.type == FrameType::AudioBegin) {
    stageAllowed = false;
    cameraPreview.pauseForConversation();
    stopCapture(captureActive);
  }
  if (frame.type == FrameType::State && frame.payload.size() == 1 && frame.payload[0] == 1 && captureActive)
    acceptedCapture = true;
  if (frame.type == FrameType::AudioStop) {
    if (frame.payload.empty()) {
      stageAllowed = false;
      cancelConversationGesture();
    } else if (frame.payload.size() == 1 && frame.payload[0] == 1) {
      stageAllowed = false;
      if (!gestures.voiceMotionActive()) cancelConversationGesture();
    }
  }
  const auto outcome = frameDispatcher->apply(frame, millis());
  if (frame.type == FrameType::AudioEnd && outcome.result == charadock::FrameApplyResult::Applied)
    stageAllowed = audioPlayback.config().expectedSamples > 0;
  if (outcome.result == charadock::FrameApplyResult::PortraitCompleted)
    idleBacklight.noteActivity(millis());
  sendProtocolOutcome(frame, outcome, &target);
}

void updateConnection() {
  const uint32_t now = millis();
  // Avoid a blocking LAN reconnect attempt starving USB microphone/playback.
  if ((!captureActive && !audioPlayback.active()) || wifi.socketConnected()) wifi.update(now);
  if (wifi.takeSocketOpened() || wifi.shouldSendHello(now))
    sendHello(wifi.transport(), ++eventSequence, charadock::protocol::FrameType::DeviceHello);
  if (usbReady) {
    if (Serial) usbMissingAt = 0;
    else if (!usbMissingAt) usbMissingAt = now;
    if (now - usbSeenAt >= 24000 || (usbMissingAt && now - usbMissingAt >= 1200)) {
      usbReady = false;
      usbMissingAt = 0;
      usbFrameStream.reset();
    }
  }
  if (wifi.hostAuthenticated() && now - wifiSeenAt >= 24000) wifi.resetHost();
  const bool nextWifi = !usbReady && wifi.hostAuthenticated();
  const bool connected = hostConnected();
  if (nextWifi != wifiActive || (!connected && wasConnected)) {
    interruptConversation(wasConnected, InterruptReason::Transport);
    hardware.setMotionEnabled(false);
    faceTracking = {};
    cameraAllowed = faceTrackingEnabled = false;
    lastCaptureControlAt = 0;
    wifiActive = nextWifi;
  }
  wasConnected = connected;
  microphoneEnabled = captureMode() != 2;
  for (const auto &frame : wifi.poll()) handleConnectedFrame(frame, true);
}

void sendProtocolInvalidFromHeader() {
  if (serialFrameBytes.size() < 8)
    return;
  charadock::protocol::Frame request;
  request.type =
      static_cast<charadock::protocol::FrameType>(serialFrameBytes[3]);
  request.sequence = static_cast<uint16_t>(serialFrameBytes[4]) |
                     static_cast<uint16_t>(serialFrameBytes[5]) << 8;
  sendProtocolOutcome(request, {charadock::FrameApplyResult::InvalidPayload,
                                charadock::PortraitCacheResult::Ok});
}

void applySerialFrame() {
  const auto frames =
      serialFrameDecoder.push(serialFrameBytes.data(), serialFrameBytes.size());
  const size_t rejected = serialFrameDecoder.rejectedFrames();
  serialRejectedFrames += rejected;
  serialFrameDecoder.reset();
  if (frames.size() != 1 || !frameDispatcher) {
    if (!rejected)
      ++serialRejectedFrames;
    sendProtocolInvalidFromHeader();
    return;
  }
  handleConnectedFrame(frames[0], false);
}

void consumeSerialFrameByte(uint8_t value) {
  serialFrameBytes.push_back(value);
  if (serialFrameBytes.size() == charadock::protocol::kHeaderBytes) {
    if (serialFrameBytes[2] != charadock::protocol::kVersion) {
      ++serialRejectedFrames;
      resetSerialFrameInput();
      return;
    }
    const size_t payloadBytes = static_cast<size_t>(serialFrameBytes[6]) |
                                static_cast<size_t>(serialFrameBytes[7]) << 8;
    if (payloadBytes > charadock::protocol::kMaximumPayloadBytes) {
      ++serialRejectedFrames;
      sendProtocolInvalidFromHeader();
      resetSerialFrameInput();
      return;
    }
    serialFrameExpectedBytes = charadock::protocol::kHeaderBytes + payloadBytes;
  }
  if (serialFrameExpectedBytes &&
      serialFrameBytes.size() == serialFrameExpectedBytes) {
    applySerialFrame();
    resetSerialFrameInput();
  }
}

void pollSerialInput() {
  if (usbReady) {
    // Keep the binary stream parser across reads. Recover a valid retry
    // after a short/truncated frame without interpreting PCM as console text.
    static uint8_t bytes[charadock::protocol::kHeaderBytes + charadock::protocol::kMaximumPayloadBytes];
    size_t count = 0;
    while (count < sizeof(bytes) && Serial.available())
      bytes[count++] = static_cast<uint8_t>(Serial.read());
    if (!count) return;
    const auto rejected = usbFrameStream.rejectedFrames();
    const auto frames = usbFrameStream.push(bytes, count);
    serialRejectedFrames += usbFrameStream.rejectedFrames() - rejected;
    for (const auto &frame : frames) handleConnectedFrame(frame, false);
    return;
  }
  const uint32_t now = millis();
  if (serialInputMode != SerialInputMode::Console &&
      now - serialFrameStartedAt >= kSerialFrameTimeoutMs) {
    ++serialRejectedFrames;
    resetSerialFrameInput();
  }
  size_t budget = charadock::protocol::kHeaderBytes +
                  charadock::protocol::kMaximumPayloadBytes;
  while (budget-- && Serial.available()) {
    const uint8_t value = static_cast<uint8_t>(Serial.read());
    if (serialInputMode == SerialInputMode::Console) {
      if (consoleLine.length() == 0 && value == 'C') {
        serialInputMode = SerialInputMode::SawMagicC;
        serialFrameStartedAt = now;
      } else {
        consumeConsoleByte(value);
      }
      continue;
    }
    if (serialInputMode == SerialInputMode::SawMagicC) {
      if (value == 'D') {
        serialInputMode = SerialInputMode::Frame;
        serialFrameBytes.clear();
        serialFrameBytes.push_back('C');
        serialFrameBytes.push_back('D');
      } else {
        serialInputMode = SerialInputMode::Console;
        consumeConsoleByte('C');
        consumeConsoleByte(value);
      }
      continue;
    }
    consumeSerialFrameByte(value);
    if (usbReady) break;
  }
}

void updateEnvelopePreview(uint32_t now) {
  if (!envelopePreviewActive)
    return;
  const uint32_t elapsed = now - envelopePreviewStartedAt;
  if (elapsed >= 1500) {
    envelopePreviewActive = false;
    const auto update = mouthEnvelope.close(now);
    if (update.changed)
      presentation.setMouthLevel(update.level);
    presentation.setState(charadock::DeviceState::Idle, now);
    Serial.println("audio envelope preview complete");
    return;
  }
  if (envelopePreviewLastBlockAt && now - envelopePreviewLastBlockAt < 20)
    return;
  envelopePreviewLastBlockAt = now;
  int16_t amplitude = 0;
  if (elapsed >= 250 && elapsed < 650)
    amplitude = 900;
  else if (elapsed >= 650 && elapsed < 1050)
    amplitude = 3200;
  else if (elapsed >= 1050 && elapsed < 1250)
    amplitude = 700;
  int16_t samples[160];
  for (size_t index = 0; index < 160; ++index)
    samples[index] = index % 2 ? amplitude : -amplitude;
  const auto update = mouthEnvelope.observePlayedSamples(samples, 160, now);
  if (update.changed)
    presentation.setMouthLevel(update.level);
}

void updateAudioPlayback(uint32_t now) {
  loopPhase.store(LoopPhase::SpeakerService);
  if (lastAudioServiceAt && audioPlayback.active())
    maxAudioServiceGap = std::max(maxAudioServiceGap, now - lastAudioServiceAt);
  lastAudioServiceAt = now;
  if (audioPlayback.generation() != observedAudioGeneration) {
    observedAudioGeneration = audioPlayback.generation();
    if (audioPlayback.active()) lastAudioUnderruns = lastSpeakerGaps = maxAudioServiceGap = 0;
    speakerWasQueued = false;
    envelopePreviewActive = false;
    loopPhase.store(LoopPhase::SpeakerStop);
    hardware.stopSpeakerStream();
    loopPhase.store(LoopPhase::SpeakerService);
    mouthEnvelope.reset(now);
    presentation.closeSpeech();
    presentation.setMouthLevel(0);
    if (presentation.snapshot().state == charadock::DeviceState::Speaking) {
      presentation.setState(charadock::DeviceState::Idle, now);
      presentation.setExpression(charadock::Expression::Neutral);
    }
  }

  // Detect a complete loss of the speaker's DMA tail while PCM is available.
  if (speakerWasQueued && audioPlayback.bufferedSamples() && !hardware.speakerPlaying()) {
    ++lastSpeakerGaps;
    speakerWasQueued = false;
  }
  // Keep both mixer slots filled before the next display flush.
  for (size_t queued = 0; queued < 2 && audioPlayback.active() &&
                          hardware.speakerQueueHasRoom(); ++queued) {
    // An empty network ring is still playable while the mixer owns a block.
    // Wait for that block before starting a full rebuffer.
    if (!audioPlayback.bufferedSamples() && !audioPlayback.producerEnded() &&
        hardware.speakerPlaying())
      break;
    int16_t samples[charadock::HardwareController::kSpeakerBlockSamples];
    const size_t count = audioPlayback.prepareSamples(
        samples, charadock::HardwareController::kSpeakerBlockSamples);
    if (!count || !hardware.queueSpeakerPcm(samples, count,
                                            audioPlayback.config().sampleRate,
                                            audioPlayback.config().volume))
      break;
    audioPlayback.commitSamples(count);
    speakerWasQueued = true;
  }

  if (audioPlayback.active()) lastAudioUnderruns = audioPlayback.underrunCount();
  const auto speakerBlock = hardware.currentSpeakerBlock();
  if (speakerBlock.samples) {
    if (observedSpeakerBlock != speakerBlock.sequence) {
      observedSpeakerBlock = speakerBlock.sequence;
      observedSpeakerBlockAt = now;
      observedSpeakerOffset = 0;
    }
    if (reportedAudioGeneration != observedAudioGeneration && hostConnected()) {
      reportedAudioGeneration = observedAudioGeneration;
      activeTransport().send(charadock::protocol::FrameType::AudioPlaybackStarted,
                             audioPlayback.beginSequence());
    }
    // Lip motion still follows 40 ms segments when the mixer gets longer blocks.
    constexpr size_t kEnvelopeSamples = 640;
    if (observedSpeakerOffset < speakerBlock.count &&
        now - observedSpeakerBlockAt >= observedSpeakerOffset / 16) {
      const size_t count = std::min(kEnvelopeSamples, speakerBlock.count - observedSpeakerOffset);
      const auto update = mouthEnvelope.observePlayedSamples(
          speakerBlock.samples + observedSpeakerOffset, count, now);
      observedSpeakerOffset += count;
      const auto cues = presentation.observeSpeech(update.rms, now);
      if (cues.emphasized) ++speechAccents;
      if (cues.moveHead && presentation.snapshot().motionProfile != charadock::MotionProfile::Reserved)
        gestures.request(charadock::HeadGesture::Accent, now);
      if (update.changed) {
        presentation.setMouthLevel(update.level);
        ++spokenMouthChanges;
      }
    }
  } else if (!speakerBlock.samples && observedSpeakerBlock) {
    observedSpeakerBlock = 0;
    mouthEnvelope.reset(now);
    presentation.closeSpeech();
    presentation.setMouthLevel(0);
  }

  if (audioPlayback.phase() == charadock::AudioPlaybackPhase::Finished && !hardware.speakerPlaying()) {
    loopPhase.store(LoopPhase::SpeakerStop);
    hardware.finishSpeakerStream();
    loopPhase.store(LoopPhase::SpeakerService);
    const auto update = mouthEnvelope.close(now);
    if (update.changed)
      presentation.setMouthLevel(update.level);
    if (presentation.snapshot().state == charadock::DeviceState::Speaking) {
      presentation.setState(charadock::DeviceState::Idle, now);
      presentation.setExpression(charadock::Expression::Neutral);
    }
    audioPlayback.stop();
  }
}

void updateFaceTracking(uint32_t now) {
  if (privacyMuted || !hardware.motionEnabled()) {
    faceTracking = {};
    return;
  }
  const bool idle = presentation.snapshot().state == charadock::DeviceState::Idle &&
      !touchWasPressed && !rawTouchPressed && !captureActive && !audioPlayback.active() &&
      !hardware.speakerPlaying() && !gestures.busy();
  if (!idle) return;
  int x = 0, y = 0;
  uint32_t seenAt;
  if (!cameraPreview.facePosition(now, seenAt, x, y)) seenAt = 0;
  faceTracking.update(now, seenAt, x, y, faceTrackingEnabled);
  hardware.setTrackingPose(faceTracking.yaw(), faceTracking.pitch());
}

void handlePrivacyTouch() {
  const auto &touch = M5.Touch.getDetail();
  if (!M5.Touch.getCount() || !touch.isPressed()) {
    privacyTouchHandled = false;
    return;
  }
  if (privacyTouchHandled || touch.base_x < 276 || touch.base_y > 43 ||
      touch.x < 276 || touch.y > 43 || millis() - touch.base_msec < 600) return;
  privacyTouchHandled = true;
  privacyMuted = !privacyMuted;
  idleBacklight.noteActivity(millis());
  if (privacyMuted) interruptConversation(false);
  Preferences settings;
  if (settings.begin("cd-privacy", false)) {
    settings.putBool("muted", privacyMuted);
    settings.end();
  }
  if (hostConnected()) {
    const uint8_t event[] = {1, 0, static_cast<uint8_t>(privacyMuted ? 7 : 8), 0, 0, 0, 0};
    activeTransport().send(charadock::protocol::FrameType::InputEvent, ++eventSequence, event, sizeof(event));
  }
}

void handleQuickSettingsTouch(uint32_t now) {
  using charadock::QuickAction;
  const auto &touch = M5.Touch.getDetail();
  const bool pressed = M5.Touch.getCount() && touch.isPressed();
  const auto action = quickSettingsTouch.update(pressed, touch.x, touch.y, quickSettingsOpen,
                                                cameraPreview.active(), !hardware.backlight());
  if (pressed) {
    idleBacklight.noteActivity(now);
    if (quickSettingsOpen) quickSettingsUsedAt = now;
  }
  if (quickSettingsOpen && now - quickSettingsUsedAt >= 12000) quickSettingsOpen = false;
  if (action == QuickAction::None) return;
  quickSettingsUsedAt = now;
  switch (action) {
  case QuickAction::Open:
    quickSettingsOpen = true;
    break;
  case QuickAction::Close:
    quickSettingsOpen = false;
    break;
  case QuickAction::PreviewBack:
    cameraPreview.stop();
    quickSettingsOpen = previewFromSettings;
    previewFromSettings = false;
    break;
  case QuickAction::Quieter:
  case QuickAction::Louder: {
    if (privacyMuted) break;
    const int step = action == QuickAction::Louder ? 10 : -10;
    const int level = std::clamp(hardware.speakerLevel() + step, 0, 100);
    if (level == hardware.speakerLevel()) break;
    hardware.setSpeakerLevel(level);
    Preferences settings;
    if (settings.begin("cd-privacy", false)) {
      settings.putUChar("volume", level);
      settings.end();
    }
    break;
  }
  case QuickAction::Tracking:
    if (cameraAllowed && !privacyMuted && hostConnected()) {
      const uint8_t event[] = {1, 0, 9, static_cast<uint8_t>(!faceTrackingEnabled), 0, 0, 0};
      if (activeTransport().send(charadock::protocol::FrameType::InputEvent, ++eventSequence, event, sizeof(event)))
        faceTrackingEnabled = !faceTrackingEnabled;
    }
    break;
  case QuickAction::Preview:
    if (startCameraPreview()) {
      previewFromSettings = true;
      quickSettingsOpen = false;
    }
    break;
  case QuickAction::None:
    break;
  }
}

void handleTouch() {
  if (privacyMuted) return;
  const uint32_t now = millis();
  const auto state = presentation.snapshot().state;
  const bool conversationActive = audioPlayback.active() || stagedPlayback.active() ||
      hardware.speakerPlaying() || state == charadock::DeviceState::Speaking ||
      state == charadock::DeviceState::Thinking;
  // A weak pad signal can persist during speaker playback. Require a firm
  // reading throughout the debounce interval before interrupting a reply.
  const bool firmRequired = conversationActive && !touchWasPressed;
  const bool rawTouched = hardware.headTouched() &&
      (!firmRequired || hardware.headTouchStrength() >= 2);
  if (rawTouched != rawTouchPressed) {
    rawTouchPressed = rawTouched;
    rawTouchChangedAt = now;
  }
  const uint32_t debounceMs = rawTouched && firmRequired ? 600u : 45u;
  const bool touched = now - rawTouchChangedAt >= debounceMs ? rawTouchPressed : touchWasPressed;
  if (touched && !touchWasPressed) {
    waitingForQuiet = false;
    quietToRearm = 0;
    cameraPreview.pauseForConversation();
    if (micMonitoring) {
      if (captureActive) interruptConversation();
      else stopCapture(false);
    }
    cancelConversationGesture();
    touchStartedAt = rawTouchChangedAt;
    pttStarted = false;
    touchInterruptedPlayback = false;
    expressionBeforeTouch = presentation.snapshot().expression;
    if (presentation.snapshot().state == charadock::DeviceState::Speaking ||
        presentation.snapshot().state == charadock::DeviceState::Thinking ||
        audioPlayback.active() || stagedPlayback.active() || hardware.speakerPlaying()) {
      if (!usbReady) Serial.printf("event interrupt touch=%u\n", hardware.headTouchStrength());
      interruptConversation(true, InterruptReason::HeadTouch);
      touchStartedAt = now;
      audioPlayback.stop();
      envelopePreviewActive = false;
      mouthEnvelope.close(now);
      presentation.setMouthLevel(0);
      presentation.setState(charadock::DeviceState::Idle, now);
      touchInterruptedPlayback = true;
    }
    presentation.setExpression(charadock::Expression::Listening);
  }
  if (touched && rawTouchPressed && !pttStarted && microphoneEnabled && now - touchStartedAt >= kLongPressMs) {
    pttStarted = true;
    if (!usbReady) Serial.println("event ptt-start");
    startCapture();
  }
  if (!touched && touchWasPressed) {
    if (pttStarted) {
      if (!usbReady) Serial.println("event ptt-end");
      const bool wasCapturing = captureActive;
      stopCapture(true);
      if (!wasCapturing && presentation.snapshot().state != charadock::DeviceState::Error)
        presentation.setExpression(expressionBeforeTouch);
    } else if (touchInterruptedPlayback) {
      presentation.setState(charadock::DeviceState::Idle, now);
      presentation.setExpression(charadock::Expression::Neutral);
    } else {
      presentation.setExpression(expressionBeforeTouch);
      if (!usbReady) Serial.println("event touch");
    }
  }
  touchWasPressed = touched;
}

} // namespace

void setup() {
  // A host can send a complete portrait or PCM frame during a display flush.
  Serial.setRxBufferSize(2 * (charadock::protocol::kMaximumPayloadBytes +
                              charadock::protocol::kHeaderBytes));
  Serial.begin(kSerialBaud);
  delay(80);
  const uint64_t mac = ESP.getEfuseMac();
  char id[32];
  snprintf(id, sizeof(id), "stackchan-%012llx",
           static_cast<unsigned long long>(mac & 0xffffffffffffULL));
  deviceId = id;

  hardware.begin();
  Preferences privacySettings;
  if (privacySettings.begin("cd-privacy", true)) {
    privacyMuted = privacySettings.getBool("muted", false);
    hardware.setSpeakerLevel(privacySettings.getUChar("volume", 100));
    privacySettings.end();
  }
  if (privacyMuted) M5.Mic.end();
  idleBacklight.noteActivity(millis());
  wifi.begin(deviceId.c_str());
  initializePortraitCache();
  initializeAudioPlayback();
  frameDispatcher = std::make_unique<charadock::FrameDispatcher>(
      presentation, portraitCache, &audioPlayback, &stagedPlayback);
  serialFrameBytes.reserve(charadock::protocol::kHeaderBytes +
                           charadock::protocol::kMaximumPayloadBytes);
  face = std::make_unique<charadock::NativeFaceRenderer>(M5.Display);
  if (!face->begin()) {
    Serial.println(
        "fatal: could not allocate the 320x240 RGB565 display buffer");
    presentation.setState(charadock::DeviceState::Error, millis());
  }
  presentation.setDisplayMode(charadock::DisplayMode::NativeFace);
  presentation.setState(charadock::DeviceState::Connecting, millis());
  Serial.printf("CharaDock StackChan %s ready (%s)\n",
                CHARADOCK_STACKCHAN_FIRMWARE_VERSION, deviceId.c_str());
  Serial.println("Servo power is OFF. Type 'help' for bring-up commands.");
  startLoopMonitor();
  printCapabilities();
  sendHello(usb, ++eventSequence, charadock::protocol::FrameType::DeviceHello);
}

void loop() {
  const uint32_t now = millis();
  lastLoopAt.store(now);
  if (audioPlayback.active() || hardware.speakerStreamOpen()) updateAudioPlayback(now);
  loopPhase.store(LoopPhase::Input);
  hardware.update();
  loopPhase.store(LoopPhase::Transport);
  pollSerialInput();
  updateConnection();
  handlePrivacyTouch();
  handleQuickSettingsTouch(millis());
  handleTouch();
  loopPhase.store(LoopPhase::Capture);
  updateCapture();
  updateAudioPlayback(millis());
  if (privacyMuted && M5.Mic.isEnabled()) M5.Mic.end();
  loopPhase.store(LoopPhase::Presentation);
  const uint32_t presentationNow = millis();
  updateEnvelopePreview(presentationNow);
  if (!hostConnected() && presentation.snapshot().state == charadock::DeviceState::Thinking &&
      presentationNow - presentation.stateChangedAt() >= kThinkingPreviewMs) {
    presentation.setState(charadock::DeviceState::Idle, presentationNow);
    presentation.setExpression(charadock::Expression::Neutral);
  }
  charadock::HeadPose pose{};
  if (!captureActive && gestures.next(presentationNow, hardware.speakerPlaying() && mouthEnvelope.lastRms() >= 450,
                                      presentation.snapshot().motionIntensity, pose)) {
    ++renderedGesturePoses;
    presentation.setFacePose(pose.yaw, pose.pitch);
    hardware.applyGesturePose(pose);
  }
  updateFaceTracking(presentationNow);
  const bool voicedOutput = hardware.speakerPlaying() && mouthEnvelope.lastRms() >= 450;
  if (voicedOutput) lastVoicedAt = presentationNow;
  if (expressionCuePending) {
    if (presentationNow - expressionCueAt >= 2500) expressionCuePending = false;
    else if (!voicedOutput && presentationNow - lastVoicedAt >= 100) expressionCueNeedsPause = false;
    else if (voicedOutput && !expressionCueNeedsPause) {
      presentation.setExpression(expressionCue);
      expressionCuePending = false;
    }
  }
  presentation.update(presentationNow);
  auto snapshot = presentation.snapshot();
  snapshot.privacyMuted = privacyMuted;
  snapshot.connection = usbReady ? "USB" : wifiActive ? "Wi-Fi" : "";
  const bool audiblePlayback = hardware.speakerPlaying();
  if (audiblePlayback) snapshot.state = charadock::DeviceState::Speaking;
  hardware.updateVoiceLed(captureActive, audiblePlayback);
  const uint32_t displayNow = millis();
  const bool facePresent = cameraPreview.recentFace(displayNow);
  snapshot.showFaceDetection(hostConnected() && cameraAllowed && captureMode() == 3, facePresent);
  if (facePresent && !faceWasPresent) idleBacklight.noteActivity(displayNow);
  faceWasPresent = facePresent;
  const int8_t gazeTarget = cameraPreview.gazeOffset(displayNow);
  if (displayNow - lastAttentionStepAt >= 30 && gazeTarget != attentionX) {
    attentionX += gazeTarget > attentionX ? 1 : -1;
    lastAttentionStepAt = displayNow;
  }
  snapshot.attentionX = attentionX;
  if (snapshot.state != previousDisplayState) {
    previousDisplayState = snapshot.state;
    idleBacklight.noteActivity(displayNow);
  }
  const bool conversationActive = snapshot.state == charadock::DeviceState::Listening ||
      snapshot.state == charadock::DeviceState::Thinking ||
      snapshot.state == charadock::DeviceState::Speaking;
  if (cameraPreview.active() && (conversationActive || displayNow - cameraStartedAt >= 15000)) {
    cameraPreview.stop();
    previewFromSettings = false;
  }
  const uint8_t brightness = idleBacklight.update(displayNow,
      touchWasPressed || captureActive || audioPlayback.active() || conversationActive ||
      cameraPreview.active() || quickSettingsOpen);
  if (cameraPreview.active()) {
    cameraPreview.draw();
  } else if (face && brightness && quickSettingsOpen) {
    const bool trackingAvailable = !privacyMuted && cameraAllowed && hostConnected();
    const bool previewAvailable = canStartCameraPreview();
    const char *trackingStatus = privacyMuted ? "一括オフ中" : !cameraAllowed ? "カメラがオフ" :
        !faceTrackingEnabled ? "オフ" : hardware.motionEnabled() ? "オン" : "オン・首の動きがオフ";
    const char *cameraStatus = privacyMuted ? "一括オフ中" : !cameraAllowed ? "カメラがオフ" :
        previewAvailable ? "表示中にタップで戻る" : "会話が終わると使えます";
    const char *connection = usbReady ? "USB" : wifiActive ? "Wi-Fi" : "未接続";
    face->renderQuickSettings(displayNow, hardware.speakerLevel(), privacyMuted, connection, trackingStatus,
                              trackingAvailable, cameraStatus, previewAvailable);
  } else if (face && brightness) {
    if (presentation.shouldRenderPortrait(now) &&
        !audiblePlayback)
      face->renderPortrait(portraitCache.pixels(), snapshot, now);
    else
      face->render(snapshot, now, (micMonitoring || audiblePlayback) ? 80 : 33);
  }
  hardware.setBacklight(brightness);
  delay(5);
}
