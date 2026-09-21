// SPDX-License-Identifier: Apache-2.0
#include <Arduino.h>
#include <ArduinoJson.h>
#include <M5Unified.hpp>
#include <esp_heap_caps.h>

#include <memory>
#include <vector>

#include "charadock/frame_dispatcher.hpp"
#include "charadock/hardware.hpp"
#include "charadock/mouth_envelope.hpp"
#include "charadock/native_face.hpp"
#include "charadock/portrait_cache.hpp"
#include "charadock/presentation.hpp"
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
constexpr size_t kAudioRingSamples = 16384;

enum class SerialInputMode : uint8_t {
  Console,
  SawMagicC,
  Frame,
};

charadock::HardwareController hardware;
charadock::PresentationController presentation;
charadock::PortraitCache portraitCache;
charadock::MouthEnvelope mouthEnvelope;
charadock::PcmPlaybackBuffer audioPlayback;
std::unique_ptr<charadock::NativeFaceRenderer> face;
std::unique_ptr<charadock::FrameDispatcher> frameDispatcher;
charadock::protocol::Decoder serialFrameDecoder;
std::vector<uint8_t> serialFrameBytes;
String consoleLine;
String deviceId;
uint8_t *portraitSlots[2] = {nullptr, nullptr};
int16_t *audioRingStorage = nullptr;
bool touchWasPressed = false;
bool pttStarted = false;
bool touchInterruptedPlayback = false;
uint32_t touchStartedAt = 0;
uint8_t demoStateIndex = 0;
charadock::Expression expressionBeforeTouch = charadock::Expression::Neutral;
bool envelopePreviewActive = false;
uint32_t envelopePreviewStartedAt = 0;
uint32_t envelopePreviewLastBlockAt = 0;
uint32_t serialFrameStartedAt = 0;
size_t serialFrameExpectedBytes = 0;
size_t serialRejectedFrames = 0;
uint32_t observedAudioGeneration = 0;
SerialInputMode serialInputMode = SerialInputMode::Console;
charadock::WifiConnection wifi;
charadock::UsbTransport usb(Serial, "usb");
uint16_t eventSequence = 0;
uint32_t usbSeenAt = 0, wifiSeenAt = 0;
bool usbReady = false, wifiActive = false, captureActive = false;
bool micPending = false;
bool wasConnected = false, microphoneEnabled = true;
bool usbMicrophoneEnabled = true, wifiMicrophoneEnabled = true;
uint32_t captureStartedAt = 0;
int16_t micBlock[320] = {};

bool hostConnected() {
  return usbReady || (wifi.hostAuthenticated() && millis() - wifiSeenAt < 24000);
}
charadock::UsbTransport &activeTransport() { return wifiActive ? wifi.transport() : usb; }
void stopCapture(bool complete) {
  const bool wasActive = captureActive;
  captureActive = false;
  M5.Mic.end();
  micPending = false;
  if (wasActive && complete && hostConnected()) {
    activeTransport().send(charadock::protocol::FrameType::PttEnd, ++eventSequence);
    presentation.setState(charadock::DeviceState::Thinking, millis());
  }
}
void interruptConversation(bool notify = true) {
  stopCapture(false);
  audioPlayback.stop();
  hardware.stopSpeakerStream();
  presentation.setMouthLevel(0);
  presentation.setState(charadock::DeviceState::Idle, millis());
  if (notify && hostConnected()) activeTransport().send(charadock::protocol::FrameType::Interrupt, ++eventSequence);
  if (face) face->setCaption("CharaDock", "");
}
void startCapture() {
  if (!hostConnected() || !microphoneEnabled) return;
  // Touch-down already interrupted any prior reply. A second asynchronous
  // host Interrupt adjacent to PttStart could cancel this new capture.
  interruptConversation(false);
  if (!M5.Mic.begin()) {
    presentation.setState(charadock::DeviceState::Error, millis());
    return;
  }
  captureActive = true;
  captureStartedAt = millis();
  presentation.setState(charadock::DeviceState::Listening, millis());
  if (!activeTransport().send(charadock::protocol::FrameType::PttStart, ++eventSequence))
    stopCapture(false);
}
void updateCapture() {
  if (!captureActive) return;
  if (!hostConnected() || millis() - captureStartedAt > 30000) {
    stopCapture(hostConnected());
    return;
  }
  if (micPending) {
    if (M5.Mic.isRecording()) return;
    micPending = false;
    if (!activeTransport().send(charadock::protocol::FrameType::PcmChunk,
          ++eventSequence, reinterpret_cast<uint8_t *>(micBlock), sizeof(micBlock))) {
      interruptConversation();
      return;
    }
  }
  micPending = M5.Mic.record(micBlock, 320, 16000);
  if (!micPending) interruptConversation();
}

void sendHello(charadock::UsbTransport &target, uint16_t sequence,
               charadock::protocol::FrameType type) {
  JsonDocument doc;
  doc["protocol"] = 2;
  doc["board"] = "m5stack-stackchan-k151";
  doc["deviceId"] = deviceId;
  doc["firmware"] = CHARADOCK_STACKCHAN_FIRMWARE_VERSION;
  doc["capabilities"]["audio"]["capture"] = true;
  doc["capabilities"]["audio"]["playback"] = audioPlayback.storageReady();
  doc["capabilities"]["audio"]["sampleRate"] = 16000;
  doc["capabilities"]["audio"]["duplex"] = "half";
  doc["capabilities"]["display"]["width"] = 320;
  doc["capabilities"]["display"]["height"] = 240;
  doc["capabilities"]["display"]["bitsPerPixel"] = 16;
  doc["capabilities"]["display"]["bitmap"].add("rgb565le");
  doc["capabilities"]["motion"]["enabled"] = hardware.motionEnabled();
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
                "underruns=%u protocol-rejected=%u\n",
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
                static_cast<unsigned>(serialRejectedFrames));
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
  audioRingStorage = static_cast<int16_t *>(
      heap_caps_malloc(kAudioRingSamples * sizeof(int16_t),
                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!audioRingStorage) {
    audioRingStorage = static_cast<int16_t *>(
        heap_caps_malloc(kAudioRingSamples * sizeof(int16_t), MALLOC_CAP_8BIT));
  }
  if (!audioRingStorage ||
      !audioPlayback.attach(audioRingStorage, kAudioRingSamples)) {
    if (audioRingStorage)
      heap_caps_free(audioRingStorage);
    audioRingStorage = nullptr;
    Serial.println("warning: PCM playback ring unavailable");
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
  } else if (name == "capabilities") {
    printCapabilities();
  } else if (name == "status") {
    printStatus();
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
    else
      ok = false;
  } else if (name == "audio") {
    if (value == "speaker-test") {
      audioPlayback.stop();
      hardware.stopSpeakerStream();
      hardware.runSpeakerTest();
    } else if (value == "mic-test") {
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
  else if (usbReady) usbSeenAt = millis();
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
    // Shared capture mode(PTT=0, disabled=2), threshold(u16).
    if (frame.payload.size() != 3 || (frame.payload[0] != 0 && frame.payload[0] != 2)) {
      reply(false); return;
    }
    (network ? wifiMicrophoneEnabled : usbMicrophoneEnabled) = frame.payload[0] == 0;
    microphoneEnabled = wifiActive ? wifiMicrophoneEnabled : usbMicrophoneEnabled;
    if (!microphoneEnabled && captureActive) interruptConversation();
    reply(true); return;
  }
  // Allow diagnostics and standby configuration on both links, but only
  // the USB-preferred owner may change conversation/display state.
  if (network && usbReady) { reply(false); return; }
  if (frame.type == FrameType::Motion) {
    if (!hostConnected() || frame.payload.size() != 1 || frame.payload[0] > 1) { reply(false); return; }
    hardware.setMotionEnabled(frame.payload[0] == 1);
    reply(true); return;
  }
  if (static_cast<uint8_t>(frame.type) == 0x60) {
    JsonDocument scene;
    if (frame.payload.size() > 1024 || deserializeJson(scene, frame.payload.data(), frame.payload.size()) ||
        !scene["caption"].is<const char *>() || !scene["name"].is<const char *>()) { reply(false); return; }
    if (face) face->setCaption(scene["name"].as<const char *>(), scene["caption"].as<const char *>());
    reply(true); return;
  }
  if (frame.type == FrameType::AudioBegin) stopCapture(false);
  sendProtocolOutcome(frame, frameDispatcher->apply(frame, millis()), &target);
}

void updateConnection() {
  const uint32_t now = millis();
  // Avoid a blocking LAN reconnect attempt starving USB microphone/playback.
  if ((!captureActive && !audioPlayback.active()) || wifi.socketConnected()) wifi.update(now);
  if (wifi.takeSocketOpened() || wifi.shouldSendHello(now))
    sendHello(wifi.transport(), ++eventSequence, charadock::protocol::FrameType::DeviceHello);
  if (usbReady && (!Serial || now - usbSeenAt >= 24000)) usbReady = false;
  if (wifi.hostAuthenticated() && now - wifiSeenAt >= 24000) wifi.resetHost();
  const bool nextWifi = !usbReady && wifi.hostAuthenticated();
  const bool connected = hostConnected();
  if (nextWifi != wifiActive || (!connected && wasConnected)) {
    interruptConversation(wasConnected);
    hardware.setMotionEnabled(false);
    wifiActive = nextWifi;
  }
  wasConnected = connected;
  microphoneEnabled = wifiActive ? wifiMicrophoneEnabled : usbMicrophoneEnabled;
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
  const uint32_t now = millis();
  if (serialInputMode != SerialInputMode::Console &&
      now - serialFrameStartedAt >= kSerialFrameTimeoutMs) {
    ++serialRejectedFrames;
    resetSerialFrameInput();
  }
  size_t budget = 512;
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
  }
}

void cycleDemoState() {
  static constexpr charadock::DeviceState states[] = {
      charadock::DeviceState::Idle,
      charadock::DeviceState::Listening,
      charadock::DeviceState::Thinking,
      charadock::DeviceState::Speaking,
  };
  demoStateIndex = (demoStateIndex + 1) % (sizeof(states) / sizeof(states[0]));
  presentation.setState(states[demoStateIndex], millis());
  if (states[demoStateIndex] == charadock::DeviceState::Speaking) {
    presentation.setMouthLevel(2);
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
  if (audioPlayback.generation() != observedAudioGeneration) {
    observedAudioGeneration = audioPlayback.generation();
    envelopePreviewActive = false;
    hardware.stopSpeakerStream();
    const auto update = mouthEnvelope.close(now);
    if (update.changed)
      presentation.setMouthLevel(update.level);
    if (presentation.snapshot().state == charadock::DeviceState::Speaking) {
      presentation.setState(charadock::DeviceState::Idle, now);
      presentation.setExpression(charadock::Expression::Neutral);
    }
  }

  if (audioPlayback.active() && hardware.speakerQueueHasRoom()) {
    int16_t samples[charadock::HardwareController::kSpeakerBlockSamples];
    const size_t count = audioPlayback.prepareSamples(
        samples, charadock::HardwareController::kSpeakerBlockSamples);
    if (count && hardware.queueSpeakerPcm(samples, count,
                                          audioPlayback.config().sampleRate,
                                          audioPlayback.config().volume)) {
      audioPlayback.commitSamples(count);
      const auto update =
          mouthEnvelope.observePlayedSamples(samples, count, now);
      if (update.changed)
        presentation.setMouthLevel(update.level);
      if (presentation.snapshot().state != charadock::DeviceState::Speaking)
        presentation.setState(charadock::DeviceState::Speaking, now);
    }
  }

  if (audioPlayback.phase() == charadock::AudioPlaybackPhase::Finished &&
      !hardware.speakerPlaying() && hardware.finishSpeakerStream()) {
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

void handleTouch() {
  const bool touched = hardware.headTouched();
  const uint32_t now = millis();
  if (touched && !touchWasPressed) {
    touchStartedAt = now;
    pttStarted = false;
    touchInterruptedPlayback = false;
    expressionBeforeTouch = presentation.snapshot().expression;
    if (presentation.snapshot().state == charadock::DeviceState::Speaking ||
        presentation.snapshot().state == charadock::DeviceState::Thinking ||
        audioPlayback.active() || hardware.speakerPlaying()) {
      Serial.println("event interrupt");
      interruptConversation();
      audioPlayback.stop();
      envelopePreviewActive = false;
      mouthEnvelope.close(now);
      presentation.setMouthLevel(0);
      presentation.setState(charadock::DeviceState::Idle, now);
      touchInterruptedPlayback = true;
    }
    presentation.setExpression(charadock::Expression::Listening);
  }
  if (touched && !pttStarted && microphoneEnabled && now - touchStartedAt >= kLongPressMs) {
    pttStarted = true;
    presentation.setState(charadock::DeviceState::Listening, now);
    Serial.println("event ptt-start");
    startCapture();
  }
  if (!touched && touchWasPressed) {
    if (pttStarted) {
      Serial.println("event ptt-end");
      stopCapture(true);
    } else if (touchInterruptedPlayback) {
      presentation.setState(charadock::DeviceState::Idle, now);
      presentation.setExpression(charadock::Expression::Neutral);
    } else {
      presentation.setExpression(expressionBeforeTouch);
      if (portraitCache.available())
        presentation.requestPortrait(now);
      else
        cycleDemoState();
      Serial.println("event portrait-request");
    }
  }
  touchWasPressed = touched;
}

} // namespace

void setup() {
  Serial.begin(kSerialBaud);
  delay(80);
  const uint64_t mac = ESP.getEfuseMac();
  char id[32];
  snprintf(id, sizeof(id), "stackchan-%012llx",
           static_cast<unsigned long long>(mac & 0xffffffffffffULL));
  deviceId = id;

  hardware.begin();
  wifi.begin(deviceId.c_str());
  initializePortraitCache();
  initializeAudioPlayback();
  frameDispatcher = std::make_unique<charadock::FrameDispatcher>(
      presentation, portraitCache, &audioPlayback);
  serialFrameBytes.reserve(charadock::protocol::kHeaderBytes +
                           charadock::protocol::kMaximumPayloadBytes);
  face = std::make_unique<charadock::NativeFaceRenderer>(M5.Display);
  if (!face->begin()) {
    Serial.println(
        "fatal: could not allocate the 320x240 RGB565 display buffer");
    presentation.setState(charadock::DeviceState::Error, millis());
  }
  presentation.setDisplayMode(charadock::DisplayMode::Hybrid);
  presentation.setState(charadock::DeviceState::Connecting, millis());
  Serial.printf("CharaDock StackChan %s ready (%s)\n",
                CHARADOCK_STACKCHAN_FIRMWARE_VERSION, deviceId.c_str());
  Serial.println("Servo power is OFF. Type 'help' for bring-up commands.");
  printCapabilities();
  sendHello(usb, ++eventSequence, charadock::protocol::FrameType::DeviceHello);
}

void loop() {
  const uint32_t now = millis();
  hardware.update();
  pollSerialInput();
  updateConnection();
  handleTouch();
  updateCapture();
  updateAudioPlayback(now);
  updateEnvelopePreview(now);
  if (!hostConnected() && presentation.snapshot().state == charadock::DeviceState::Thinking &&
      now - presentation.stateChangedAt() >= kThinkingPreviewMs) {
    presentation.setState(charadock::DeviceState::Idle, now);
    presentation.setExpression(charadock::Expression::Neutral);
  }
  presentation.update(now);
  hardware.applyPresentation(presentation.snapshot(), now);
  if (face) {
    if (presentation.shouldRenderPortrait(now))
      face->renderPortrait(portraitCache.pixels(), presentation.snapshot(),
                           now);
    else
      face->render(presentation.snapshot(), now);
  }
  delay(5);
}
