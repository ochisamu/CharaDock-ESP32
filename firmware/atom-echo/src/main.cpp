// SPDX-License-Identifier: Apache-2.0
#include <Arduino.h>
#include <ArduinoJson.h>
#include <M5Atom.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiUdp.h>
#include <driver/i2s.h>
#include <mbedtls/md.h>
#include <math.h>


namespace {

constexpr uint32_t kSerialBaud = 500000;
constexpr char kFirmwareVersion[] = "0.5.2-handsfree-vad-jitterbuf";
constexpr uint32_t kAudioSampleRate = 16000;
constexpr uint16_t kDiscoveryPort = 41721;
constexpr uint16_t kDiscoveryLocalPort = 41723;
constexpr char kDiscoveryPrefix[] = "CHARADOCK_ATOM_DISCOVER_V1 ";
constexpr char kHostPrefix[] = "CHARADOCK_ATOM_HOST_V1 ";
constexpr i2s_port_t kI2sPort = I2S_NUM_0;
constexpr gpio_num_t kI2sBckPin = GPIO_NUM_19;
constexpr gpio_num_t kI2sLrckPin = GPIO_NUM_33;
constexpr gpio_num_t kI2sDataOutPin = GPIO_NUM_22;
constexpr gpio_num_t kI2sDataInPin = GPIO_NUM_23;
constexpr gpio_num_t kButtonPin = GPIO_NUM_39;
constexpr size_t kFrameHeaderBytes = 12;
constexpr size_t kMaximumPayloadBytes = 2048;
constexpr size_t kMicrophoneChunkBytes = 1024;
constexpr size_t kVadPrerollBytes = 6144;
constexpr uint8_t kVadSpeechStartChunks = 2;
constexpr uint8_t kVadSilenceEndChunks = 25;
constexpr uint8_t kVadCalibrationChunks = 5;
constexpr uint32_t kVadDefaultMinimumStartRms = 120;
constexpr uint32_t kVadMinimumConfigurableStartRms = 80;
constexpr uint32_t kVadMaximumConfigurableStartRms = 800;
constexpr uint32_t kVadMinimumContinueRms = 60;
constexpr uint32_t kVadMaximumThresholdRms = 4000;
constexpr uint32_t kVadStatusIntervalMs = 1000;
constexpr uint32_t kHandsFreeResumeDelayMs = 350;
constexpr uint32_t kHandsFreeMaximumUtteranceMs = 30000;
constexpr size_t kSpeakerPrebufferCapacityBytes = 8192;
constexpr uint32_t kSpeakerUsbPrebufferMs = 120;
constexpr uint32_t kSpeakerWifiPrebufferMs = 200;
constexpr uint32_t kSpeakerDrainMs = 220;
constexpr uint8_t kProtocolVersion = 1;

enum class FrameType : uint8_t {
  DeviceHello = 0x01,
  HostHello = 0x02,
  AuthChallenge = 0x03,
  DeviceAuth = 0x04,
  PttStart = 0x10,
  PcmChunk = 0x11,
  PttEnd = 0x12,
  Interrupt = 0x13,
  State = 0x20,
  AudioBegin = 0x21,
  AudioChunk = 0x22,
  AudioEnd = 0x23,
  AudioStop = 0x24,
  WifiConfig = 0x30,
  WifiStatus = 0x31,
  CaptureConfig = 0x32,
  CaptureStatus = 0x33,
  Ack = 0x7e,
  Error = 0x7f,
};

enum class DeviceState : uint8_t {
  Idle = 0,
  Listening = 1,
  Thinking = 2,
  Speaking = 3,
  Error = 4,
  Connecting = 5,
};

enum class AudioMode : uint8_t { Off, Microphone, Speaker };
enum class Transport : uint8_t { None, Usb, Wifi };

struct ReceiveState {
  uint8_t buffer[kFrameHeaderBytes + kMaximumPayloadBytes];
  size_t length = 0;
};

ReceiveState usbReceive;
ReceiveState wifiReceive;
uint8_t microphoneBuffer[kMicrophoneChunkBytes];
uint8_t vadPreroll[kVadPrerollBytes];
uint8_t speakerPrebuffer[kSpeakerPrebufferCapacityBytes];
int16_t speakerStereoBuffer[kMaximumPayloadBytes];
uint16_t transmitSequence = 0;
AudioMode audioMode = AudioMode::Off;
DeviceState deviceState = DeviceState::Connecting;
Transport activeTransport = Transport::None;
Transport recordingTransport = Transport::None;
Transport playbackTransport = Transport::None;
bool usbHostConnected = false;
bool wifiHostConnected = false;
bool recording = false;
bool handsFreeEnabled = false;
bool handsFreeMonitoring = false;
bool handsFreeButtonForced = false;
bool playing = false;
bool speakerPrimed = false;
size_t speakerPrebufferLength = 0;
size_t speakerPrebufferTargetBytes = 0;
bool suppressButtonUntilRelease = false;
bool stableButtonPressed = false;
bool lastRawButtonPressed = false;
bool wifiUdpStarted = false;
bool wifiConnectedReported = false;
uint32_t rawButtonChangedAt = 0;
uint32_t handsFreeResumeAt = 0;
uint32_t handsFreeUtteranceStartedAt = 0;
uint32_t vadNoiseFloorRms = 60;
uint32_t vadMinimumStartRms = kVadDefaultMinimumStartRms;
uint32_t vadCalibrationMinimumRms = UINT32_MAX;
uint32_t lastVadStatusAt = 0;
size_t vadPrerollLength = 0;
uint8_t vadSpeechChunks = 0;
uint8_t vadSilenceChunks = 0;
uint8_t vadCalibrationChunksRemaining = 0;
uint32_t lastUsbHelloAt = 0;
uint32_t lastWifiHelloAt = 0;
uint32_t lastWifiAttemptAt = 0;
uint32_t lastDiscoveryAt = 0;
uint32_t wifiAttemptStartedAt = 0;
String wifiSsid;
String wifiPassword;
String pairingToken;
String deviceId;
Preferences storage;
WiFiClient wifiClient;
WiFiUDP discoveryUdp;

bool transportConnected(Transport transport) {
  if (transport == Transport::Usb) return usbHostConnected;
  if (transport == Transport::Wifi) return wifiHostConnected && wifiClient.connected();
  return false;
}

uint32_t crc32(const uint8_t* bytes, size_t length, uint32_t initial = 0xffffffffu) {
  uint32_t crc = initial;
  for (size_t index = 0; index < length; ++index) {
    crc ^= bytes[index];
    for (uint8_t bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1u) ? 0xedb88320u : 0u);
  }
  return crc;
}

uint32_t frameCrc(const uint8_t* header, const uint8_t* payload, size_t payloadLength) {
  uint32_t crc = crc32(header + 2, 6);
  crc = crc32(payload, payloadLength, crc);
  return crc ^ 0xffffffffu;
}

void setLed(DeviceState state) {
  CRGB color;
  switch (state) {
    case DeviceState::Idle: color = CRGB(0, 28, 8); break;
    case DeviceState::Listening: color = CRGB(0, 28, 96); break;
    case DeviceState::Thinking: color = CRGB(80, 30, 0); break;
    case DeviceState::Speaking: color = CRGB(72, 0, 72); break;
    case DeviceState::Error: color = CRGB(96, 0, 0); break;
    case DeviceState::Connecting: color = CRGB(40, 28, 0); break;
  }
  M5.dis.drawpix(0, color);
}

void setState(DeviceState state) {
  deviceState = state;
  setLed(state);
}

size_t writeTransport(Transport transport, const uint8_t* bytes, size_t length) {
  if (transport == Transport::Usb) return Serial.write(bytes, length);
  if (transport == Transport::Wifi && wifiClient.connected()) return wifiClient.write(bytes, length);
  return 0;
}

void writeFrameTo(Transport transport, FrameType type, const uint8_t* payload = nullptr, uint16_t payloadLength = 0) {
  if (transport == Transport::None || payloadLength > kMaximumPayloadBytes) return;
  uint8_t header[kFrameHeaderBytes] = {'C', 'D', kProtocolVersion, static_cast<uint8_t>(type)};
  const uint16_t sequence = ++transmitSequence;
  header[4] = sequence & 0xff;
  header[5] = sequence >> 8;
  header[6] = payloadLength & 0xff;
  header[7] = payloadLength >> 8;
  const uint32_t checksum = frameCrc(header, payload, payloadLength);
  header[8] = checksum & 0xff;
  header[9] = (checksum >> 8) & 0xff;
  header[10] = (checksum >> 16) & 0xff;
  header[11] = (checksum >> 24) & 0xff;
  writeTransport(transport, header, sizeof(header));
  if (payload && payloadLength) writeTransport(transport, payload, payloadLength);
}

void writeFrame(FrameType type, const uint8_t* payload = nullptr, uint16_t payloadLength = 0) {
  writeFrameTo(activeTransport, type, payload, payloadLength);
}

void acknowledge(Transport transport, FrameType type, uint16_t sequence) {
  const uint8_t payload[3] = {
    static_cast<uint8_t>(type),
    static_cast<uint8_t>(sequence & 0xff),
    static_cast<uint8_t>(sequence >> 8),
  };
  writeFrameTo(transport, FrameType::Ack, payload, sizeof(payload));
}

void sendError(Transport transport, const char* message) {
  const size_t length = min(strlen(message), kMaximumPayloadBytes);
  writeFrameTo(transport, FrameType::Error, reinterpret_cast<const uint8_t*>(message), static_cast<uint16_t>(length));
  setState(DeviceState::Error);
}

void sendHello(Transport transport) {
  JsonDocument doc;
  doc["board"] = "atom-echo";
  doc["firmware"] = kFirmwareVersion;
  doc["deviceId"] = deviceId;
  doc["sampleRate"] = kAudioSampleRate;
  doc["transport"] = transport == Transport::Wifi ? "wifi" : "usb-serial";
  uint8_t payload[256];
  const size_t length = serializeJson(doc, payload, sizeof(payload));
  writeFrameTo(transport, FrameType::DeviceHello, payload, static_cast<uint16_t>(length));
  if (transport == Transport::Wifi) lastWifiHelloAt = millis();
  else lastUsbHelloAt = millis();
}

void sendWifiStatus(Transport transport, const char* phase, const char* error = "") {
  JsonDocument doc;
  doc["configured"] = wifiSsid.length() > 0 && pairingToken.length() == 64;
  doc["connected"] = WiFi.status() == WL_CONNECTED;
  doc["deviceId"] = deviceId;
  doc["ssid"] = wifiSsid;
  doc["phase"] = phase;
  if (WiFi.status() == WL_CONNECTED) doc["ip"] = WiFi.localIP().toString();
  if (error && error[0]) doc["error"] = error;
  uint8_t payload[384];
  const size_t length = serializeJson(doc, payload, sizeof(payload));
  writeFrameTo(transport, FrameType::WifiStatus, payload, static_cast<uint16_t>(length));
}

void stopAudio() {
  if (audioMode != AudioMode::Off) {
    if (audioMode == AudioMode::Speaker) i2s_zero_dma_buffer(kI2sPort);
    i2s_driver_uninstall(kI2sPort);
  }
  audioMode = AudioMode::Off;
  speakerPrimed = false;
  speakerPrebufferLength = 0;
  speakerPrebufferTargetBytes = 0;
}

bool startAudio(AudioMode mode, uint32_t sampleRate = kAudioSampleRate) {
  stopAudio();
  i2s_config_t config = {};
  config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER);
  config.sample_rate = sampleRate;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  config.channel_format = mode == AudioMode::Speaker ? I2S_CHANNEL_FMT_RIGHT_LEFT : I2S_CHANNEL_FMT_ALL_RIGHT;
  config.communication_format = I2S_COMM_FORMAT_I2S;
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  config.dma_buf_count = mode == AudioMode::Speaker ? 16 : 8;
  config.dma_buf_len = mode == AudioMode::Speaker ? 256 : 128;
  if (mode == AudioMode::Microphone) {
    config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM);
  } else {
    config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX);
    config.use_apll = false;
    config.tx_desc_auto_clear = true;
  }
  if (i2s_driver_install(kI2sPort, &config, 0, nullptr) != ESP_OK) return false;
  i2s_pin_config_t pins = {};
  pins.bck_io_num = kI2sBckPin;
  pins.ws_io_num = kI2sLrckPin;
  pins.data_out_num = kI2sDataOutPin;
  pins.data_in_num = kI2sDataInPin;
  if (i2s_set_pin(kI2sPort, &pins) != ESP_OK
      || i2s_set_clk(kI2sPort, sampleRate, I2S_BITS_PER_SAMPLE_16BIT,
                     mode == AudioMode::Speaker ? I2S_CHANNEL_STEREO : I2S_CHANNEL_MONO) != ESP_OK) {
    i2s_driver_uninstall(kI2sPort);
    return false;
  }
  audioMode = mode;
  if (mode == AudioMode::Speaker) {
    const uint32_t prebufferMs = playbackTransport == Transport::Wifi ? kSpeakerWifiPrebufferMs : kSpeakerUsbPrebufferMs;
    const size_t requested = static_cast<size_t>(sampleRate) * sizeof(int16_t) * prebufferMs / 1000;
    speakerPrebufferTargetBytes = min(kSpeakerPrebufferCapacityBytes, max<size_t>(1024, requested));
  }
  return true;
}

bool writeSpeakerPcm(const uint8_t* bytes, size_t length) {
  if (!bytes || !length || (length & 1u) || audioMode != AudioMode::Speaker) return false;
  size_t offset = 0;
  while (offset < length) {
    const size_t monoSamples = min((length - offset) / sizeof(int16_t), sizeof(speakerStereoBuffer) / (sizeof(int16_t) * 2));
    for (size_t index = 0; index < monoSamples; ++index) {
      const size_t byteIndex = offset + index * 2;
      const int16_t sample = static_cast<int16_t>(bytes[byteIndex] | (static_cast<uint16_t>(bytes[byteIndex + 1]) << 8));
      speakerStereoBuffer[index * 2] = sample;
      speakerStereoBuffer[index * 2 + 1] = sample;
    }
    const size_t stereoBytes = monoSamples * sizeof(int16_t) * 2;
    size_t written = 0;
    if (i2s_write(kI2sPort, speakerStereoBuffer, stereoBytes, &written, portMAX_DELAY) != ESP_OK || written != stereoBytes) return false;
    offset += monoSamples * sizeof(int16_t);
  }
  return true;
}

bool flushSpeakerPrebuffer() {
  if (!speakerPrebufferLength) {
    speakerPrimed = true;
    return true;
  }
  const bool written = writeSpeakerPcm(speakerPrebuffer, speakerPrebufferLength);
  speakerPrebufferLength = 0;
  speakerPrimed = written;
  return written;
}

bool queueSpeakerPcm(const uint8_t* bytes, size_t length) {
  if (!bytes || !length || (length & 1u)) return false;
  if (!speakerPrimed) {
    if (speakerPrebufferLength + length <= sizeof(speakerPrebuffer)) {
      memcpy(speakerPrebuffer + speakerPrebufferLength, bytes, length);
      speakerPrebufferLength += length;
      if (speakerPrebufferLength < speakerPrebufferTargetBytes) return true;
      return flushSpeakerPrebuffer();
    }
    if (!flushSpeakerPrebuffer()) return false;
  }
  return writeSpeakerPcm(bytes, length);
}

void resetVoiceDetection() {
  vadPrerollLength = 0;
  vadSpeechChunks = 0;
  vadSilenceChunks = 0;
  vadCalibrationChunksRemaining = kVadCalibrationChunks;
  vadCalibrationMinimumRms = UINT32_MAX;
  handsFreeUtteranceStartedAt = 0;
}

void scheduleHandsFreeResume() {
  if (handsFreeEnabled && transportConnected(activeTransport)) {
    handsFreeResumeAt = millis() + kHandsFreeResumeDelayMs;
  }
}

void stopHandsFreeMonitoring() {
  if (!handsFreeMonitoring) return;
  handsFreeMonitoring = false;
  handsFreeButtonForced = false;
  if (audioMode == AudioMode::Microphone) stopAudio();
  recordingTransport = Transport::None;
  resetVoiceDetection();
}

void stopPlayback(bool interrupted) {
  if (!playing) return;
  playing = false;
  stopAudio();
  if (interrupted) writeFrameTo(playbackTransport, FrameType::Interrupt);
  playbackTransport = Transport::None;
  setState(transportConnected(activeTransport) ? DeviceState::Idle : DeviceState::Connecting);
  scheduleHandsFreeResume();
}

void startRecording() {
  if (!transportConnected(activeTransport) || recording || playing) return;
  if (!startAudio(AudioMode::Microphone)) {
    sendError(activeTransport, "microphone init failed");
    return;
  }
  recordingTransport = activeTransport;
  recording = true;
  setState(DeviceState::Listening);
  writeFrameTo(recordingTransport, FrameType::PttStart);
}

bool startHandsFreeMonitoring() {
  if (!handsFreeEnabled || handsFreeMonitoring || recording || playing || !transportConnected(activeTransport)) return false;
  if (!startAudio(AudioMode::Microphone)) {
    sendError(activeTransport, "microphone init failed");
    return false;
  }
  recordingTransport = activeTransport;
  handsFreeMonitoring = true;
  resetVoiceDetection();
  setState(DeviceState::Listening);
  return true;
}

void beginHandsFreeUtterance(bool forcedByButton = false) {
  if (!handsFreeMonitoring || recording || recordingTransport == Transport::None) return;
  recording = true;
  M5.dis.drawpix(0, CRGB(0, 80, 80));
  handsFreeButtonForced = forcedByButton;
  handsFreeUtteranceStartedAt = millis();
  vadSilenceChunks = 0;
  writeFrameTo(recordingTransport, FrameType::PttStart);
  for (size_t offset = 0; offset < vadPrerollLength; offset += kMicrophoneChunkBytes) {
    const size_t length = min(kMicrophoneChunkBytes, vadPrerollLength - offset);
    writeFrameTo(recordingTransport, FrameType::PcmChunk, vadPreroll + offset, static_cast<uint16_t>(length));
  }
  vadPrerollLength = 0;
}

void stopRecording() {
  if (!recording) return;
  recording = false;
  handsFreeMonitoring = false;
  handsFreeButtonForced = false;
  stopAudio();
  writeFrameTo(recordingTransport, FrameType::PttEnd);
  recordingTransport = Transport::None;
  resetVoiceDetection();
  handsFreeResumeAt = 0;
  setState(DeviceState::Thinking);
}

bool validPairingToken(const char* token) {
  if (!token || strlen(token) != 64) return false;
  for (size_t index = 0; index < 64; ++index) {
    const char value = token[index];
    if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'))) return false;
  }
  return true;
}

uint8_t hexNibble(char value) {
  if (value >= '0' && value <= '9') return static_cast<uint8_t>(value - '0');
  return static_cast<uint8_t>(value - 'a' + 10);
}

bool createAuthenticationProof(const uint8_t* challenge, size_t challengeLength, uint8_t output[32]) {
  if (!validPairingToken(pairingToken.c_str())) return false;
  uint8_t key[32];
  for (size_t index = 0; index < 32; ++index) {
    key[index] = static_cast<uint8_t>((hexNibble(pairingToken[index * 2]) << 4) | hexNibble(pairingToken[index * 2 + 1]));
  }
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  return info && mbedtls_md_hmac(info, key, sizeof(key), challenge, challengeLength, output) == 0;
}

void saveWifiConfiguration(const char* ssid, const char* password, const char* token) {
  storage.begin("charadock", false);
  storage.putString("ssid", ssid);
  storage.putString("password", password);
  storage.putString("pairing", token);
  storage.end();
  wifiSsid = ssid;
  wifiPassword = password;
  pairingToken = token;
}

void loadWifiConfiguration() {
  storage.begin("charadock", true);
  wifiSsid = storage.getString("ssid", "");
  wifiPassword = storage.getString("password", "");
  pairingToken = storage.getString("pairing", "");
  storage.end();
  if (!validPairingToken(pairingToken.c_str())) pairingToken = "";
}

void beginWifiAttempt() {
  if (!wifiSsid.length() || pairingToken.length() != 64) return;
  wifiClient.stop();
  wifiHostConnected = false;
  WiFi.disconnect();
  WiFi.setSleep(false);
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  lastWifiAttemptAt = millis();
  wifiAttemptStartedAt = lastWifiAttemptAt;
  wifiConnectedReported = false;
  if (!recording && !handsFreeMonitoring && !playing && activeTransport == Transport::None) setState(DeviceState::Connecting);
}

void resetWifiTransport() {
  const bool wasActive = activeTransport == Transport::Wifi;
  wifiClient.stop();
  wifiHostConnected = false;
  wifiReceive.length = 0;
  if (wasActive) {
    if (recording || handsFreeMonitoring || playing) {
      recording = false;
      handsFreeMonitoring = false;
      handsFreeButtonForced = false;
      playing = false;
      recordingTransport = Transport::None;
      playbackTransport = Transport::None;
      stopAudio();
      resetVoiceDetection();
    }
    activeTransport = usbHostConnected ? Transport::Usb : Transport::None;
    setState(activeTransport == Transport::None ? DeviceState::Connecting : DeviceState::Idle);
  }
}

void handleWifiConfig(Transport transport, uint16_t sequence, const uint8_t* payload, uint16_t payloadLength) {
  if (transport != Transport::Usb) return;
  JsonDocument doc;
  const DeserializationError result = deserializeJson(doc, payload, payloadLength);
  const char* ssid = doc["ssid"] | "";
  const char* password = doc["password"] | "";
  const char* token = doc["token"] | "";
  if (result || !ssid[0] || strlen(ssid) > 32 || strlen(password) > 64 || !validPairingToken(token)) {
    acknowledge(transport, FrameType::WifiConfig, sequence);
    sendWifiStatus(transport, "error", "invalid Wi-Fi configuration");
    return;
  }
  saveWifiConfiguration(ssid, password, token);
  acknowledge(transport, FrameType::WifiConfig, sequence);
  sendWifiStatus(transport, "connecting");
  lastWifiAttemptAt = 0;
  beginWifiAttempt();
}

void handleHostFrame(Transport transport, FrameType type, uint16_t sequence, const uint8_t* payload, uint16_t payloadLength) {
  switch (type) {
    case FrameType::HostHello:
      {
      const Transport previousTransport = activeTransport;
      if (transport == Transport::Wifi) {
        wifiHostConnected = true;
        activeTransport = Transport::Wifi;
      } else {
        usbHostConnected = true;
        if (!wifiHostConnected) activeTransport = Transport::Usb;
      }
      if (previousTransport != Transport::None && previousTransport != activeTransport) {
        if (recording) stopRecording();
        else stopHandsFreeMonitoring();
      }
      sendHello(transport);
      if (!recording && !handsFreeMonitoring && !playing) {
        setState(DeviceState::Idle);
        scheduleHandsFreeResume();
      }
      break;
      }
    case FrameType::AuthChallenge: {
      if (transport != Transport::Wifi || payloadLength != 32) break;
      uint8_t proof[32];
      if (createAuthenticationProof(payload, payloadLength, proof)) writeFrameTo(transport, FrameType::DeviceAuth, proof, sizeof(proof));
      break;
    }
    case FrameType::WifiConfig:
      handleWifiConfig(transport, sequence, payload, payloadLength);
      break;
    case FrameType::CaptureConfig: {
      if (transport != activeTransport || (payloadLength != 1 && payloadLength != 3) || payload[0] > 1) {
        acknowledge(transport, type, sequence);
        break;
      }
      const bool enableHandsFree = payload[0] == 1;
      if (payloadLength == 3) {
        const uint32_t requestedThreshold = static_cast<uint32_t>(payload[1]) | (static_cast<uint32_t>(payload[2]) << 8);
        vadMinimumStartRms = max(kVadMinimumConfigurableStartRms, min(kVadMaximumConfigurableStartRms, requestedThreshold));
        if (handsFreeMonitoring) resetVoiceDetection();
      }
      if (handsFreeEnabled != enableHandsFree) {
        handsFreeEnabled = enableHandsFree;
        if (!handsFreeEnabled) {
          if (recording) stopRecording();
          else stopHandsFreeMonitoring();
          setState(transportConnected(activeTransport) ? DeviceState::Idle : DeviceState::Connecting);
        } else if (!recording && !playing) {
          setState(DeviceState::Idle);
          scheduleHandsFreeResume();
        }
      }
      acknowledge(transport, type, sequence);
      break;
    }
    case FrameType::State:
      if (transport == activeTransport && payloadLength == 1 && !recording && !handsFreeMonitoring && !playing
          && payload[0] <= static_cast<uint8_t>(DeviceState::Connecting)) {
        setState(static_cast<DeviceState>(payload[0]));
        if (deviceState == DeviceState::Idle) scheduleHandsFreeResume();
      }
      break;
    case FrameType::AudioBegin: {
      if (transport != activeTransport) break;
      if (payloadLength != 8) {
        acknowledge(transport, type, sequence);
        break;
      }
      const uint32_t sampleRate = static_cast<uint32_t>(payload[0])
          | (static_cast<uint32_t>(payload[1]) << 8)
          | (static_cast<uint32_t>(payload[2]) << 16)
          | (static_cast<uint32_t>(payload[3]) << 24);
      if (recording) stopRecording();
      else stopHandsFreeMonitoring();
      playbackTransport = transport;
      playing = startAudio(AudioMode::Speaker, sampleRate);
      setState(playing ? DeviceState::Speaking : DeviceState::Error);
      acknowledge(transport, type, sequence);
      break;
    }
    case FrameType::AudioChunk: {
      if (transport == playbackTransport && playing && payloadLength) {
        if (audioMode == AudioMode::Speaker && !queueSpeakerPcm(payload, payloadLength)) {
          stopPlayback(false);
          sendError(transport, "speaker write failed");
        }
      }
      acknowledge(transport, type, sequence);
      break;
    }
    case FrameType::AudioEnd:
      if (transport == playbackTransport) {
        if (playing && audioMode == AudioMode::Speaker) {
          if (!speakerPrimed) flushSpeakerPrebuffer();
          delay(kSpeakerDrainMs);
        }
        playing = false;
        playbackTransport = Transport::None;
        stopAudio();
        setState(transportConnected(activeTransport) ? DeviceState::Idle : DeviceState::Connecting);
        scheduleHandsFreeResume();
      }
      acknowledge(transport, type, sequence);
      break;
    case FrameType::AudioStop:
      if (transport == playbackTransport) stopPlayback(false);
      acknowledge(transport, type, sequence);
      break;
    default:
      break;
  }
}

void consumeHostFrames(Transport transport, Stream& stream, ReceiveState& state) {
  while (stream.available() && state.length < sizeof(state.buffer)) {
    state.buffer[state.length++] = static_cast<uint8_t>(stream.read());
  }
  while (state.length >= kFrameHeaderBytes) {
    size_t magicIndex = 0;
    while (magicIndex + 1 < state.length
           && !(state.buffer[magicIndex] == 'C' && state.buffer[magicIndex + 1] == 'D')) ++magicIndex;
    if (magicIndex) {
      memmove(state.buffer, state.buffer + magicIndex, state.length - magicIndex);
      state.length -= magicIndex;
    }
    if (state.length < kFrameHeaderBytes) return;
    if (state.buffer[2] != kProtocolVersion) {
      memmove(state.buffer, state.buffer + 2, state.length - 2);
      state.length -= 2;
      continue;
    }
    const uint16_t payloadLength = state.buffer[6] | (static_cast<uint16_t>(state.buffer[7]) << 8);
    if (payloadLength > kMaximumPayloadBytes) {
      memmove(state.buffer, state.buffer + 2, state.length - 2);
      state.length -= 2;
      continue;
    }
    const size_t frameLength = kFrameHeaderBytes + payloadLength;
    if (state.length < frameLength) return;
    const uint32_t expected = static_cast<uint32_t>(state.buffer[8])
        | (static_cast<uint32_t>(state.buffer[9]) << 8)
        | (static_cast<uint32_t>(state.buffer[10]) << 16)
        | (static_cast<uint32_t>(state.buffer[11]) << 24);
    if (expected == frameCrc(state.buffer, state.buffer + kFrameHeaderBytes, payloadLength)) {
      const uint16_t sequence = state.buffer[4] | (static_cast<uint16_t>(state.buffer[5]) << 8);
      handleHostFrame(transport, static_cast<FrameType>(state.buffer[3]), sequence,
                      state.buffer + kFrameHeaderBytes, payloadLength);
      memmove(state.buffer, state.buffer + frameLength, state.length - frameLength);
      state.length -= frameLength;
    } else {
      memmove(state.buffer, state.buffer + 2, state.length - 2);
      state.length -= 2;
    }
  }
  if (state.length == sizeof(state.buffer)) state.length = 0;
}

void discoverCharaDock() {
  if (!wifiUdpStarted) wifiUdpStarted = discoveryUdp.begin(kDiscoveryLocalPort) == 1;
  if (!wifiUdpStarted) return;
  const uint32_t now = millis();
  if (now - lastDiscoveryAt >= 1500) {
    const String request = String(kDiscoveryPrefix) + deviceId;
    discoveryUdp.beginPacket(IPAddress(255, 255, 255, 255), kDiscoveryPort);
    discoveryUdp.write(reinterpret_cast<const uint8_t*>(request.c_str()), request.length());
    discoveryUdp.endPacket();
    lastDiscoveryAt = now;
  }
  const int packetBytes = discoveryUdp.parsePacket();
  if (packetBytes <= 0) return;
  char response[96] = {};
  const int length = discoveryUdp.read(response, sizeof(response) - 1);
  if (length <= 0 || strncmp(response, kHostPrefix, strlen(kHostPrefix)) != 0) return;
  const int port = atoi(response + strlen(kHostPrefix));
  if (port < 1024 || port > 65535) return;
  const IPAddress host = discoveryUdp.remoteIP();
  if (!wifiClient.connect(host, static_cast<uint16_t>(port), 1200)) return;
  wifiClient.setNoDelay(true);
  wifiHostConnected = false;
  wifiReceive.length = 0;
  sendHello(Transport::Wifi);
}

void updateWifi() {
  if (!wifiSsid.length() || pairingToken.length() != 64) return;
  const uint32_t now = millis();
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiClient.connected() || wifiHostConnected) resetWifiTransport();
    if (!lastWifiAttemptAt || now - lastWifiAttemptAt >= 15000) beginWifiAttempt();
    if (wifiAttemptStartedAt && now - wifiAttemptStartedAt >= 12000 && usbHostConnected) {
      sendWifiStatus(Transport::Usb, "error", "Wi-Fi connection failed");
      wifiAttemptStartedAt = 0;
    }
    return;
  }
  if (!wifiConnectedReported) {
    wifiConnectedReported = true;
    if (usbHostConnected) sendWifiStatus(Transport::Usb, "connected");
  }
  if (!wifiClient.connected()) {
    if (wifiHostConnected) resetWifiTransport();
    discoverCharaDock();
    return;
  }
  consumeHostFrames(Transport::Wifi, wifiClient, wifiReceive);
  if (!wifiHostConnected && now - lastWifiHelloAt >= 1000) sendHello(Transport::Wifi);
}

void updateButton() {
  const bool rawPressed = digitalRead(kButtonPin) == LOW;
  const uint32_t now = millis();
  if (rawPressed != lastRawButtonPressed) {
    lastRawButtonPressed = rawPressed;
    rawButtonChangedAt = now;
  }
  if (rawPressed == stableButtonPressed || now - rawButtonChangedAt < 18) return;
  stableButtonPressed = rawPressed;
  if (stableButtonPressed) {
    if (playing || deviceState == DeviceState::Thinking || deviceState == DeviceState::Speaking) {
      const bool wasPlaying = playing;
      stopPlayback(true);
      if (!wasPlaying) writeFrame(FrameType::Interrupt);
      suppressButtonUntilRelease = true;
    } else if (handsFreeEnabled) {
      if (!handsFreeMonitoring) startHandsFreeMonitoring();
      beginHandsFreeUtterance(true);
    } else {
      startRecording();
    }
  } else {
    if (suppressButtonUntilRelease) suppressButtonUntilRelease = false;
    else if (!handsFreeEnabled || handsFreeButtonForced) stopRecording();
  }
}

uint32_t microphoneRms(const uint8_t* bytes, size_t length) {
  const size_t sampleCount = length / sizeof(int16_t);
  if (!bytes || !sampleCount) return 0;
  int64_t sum = 0;
  uint64_t sumSquares = 0;
  for (size_t index = 0; index < sampleCount; ++index) {
    const size_t offset = index * 2;
    const int16_t sample = static_cast<int16_t>(bytes[offset] | (static_cast<uint16_t>(bytes[offset + 1]) << 8));
    sum += sample;
    sumSquares += static_cast<int64_t>(sample) * sample;
  }
  const int64_t mean = sum / static_cast<int64_t>(sampleCount);
  const uint64_t meanSquare = sumSquares / sampleCount;
  const uint64_t dcSquare = static_cast<uint64_t>(mean * mean);
  return static_cast<uint32_t>(sqrt(static_cast<double>(meanSquare > dcSquare ? meanSquare - dcSquare : 0)));
}

uint32_t voiceStartThresholdRms() {
  const uint32_t margin = max<uint32_t>(60, vadNoiseFloorRms * 3 / 4);
  return min(kVadMaximumThresholdRms, max(vadMinimumStartRms, vadNoiseFloorRms + margin));
}

uint32_t voiceContinueThresholdRms() {
  const uint32_t margin = max<uint32_t>(35, vadNoiseFloorRms / 3);
  const uint32_t configuredMinimum = max(kVadMinimumContinueRms, vadMinimumStartRms * 3 / 4);
  return min(kVadMaximumThresholdRms, max(configuredMinimum, vadNoiseFloorRms + margin));
}

void writeUint16(uint8_t* output, size_t offset, uint32_t value) {
  const uint16_t bounded = static_cast<uint16_t>(min<uint32_t>(UINT16_MAX, value));
  output[offset] = static_cast<uint8_t>(bounded & 0xff);
  output[offset + 1] = static_cast<uint8_t>(bounded >> 8);
}

void sendVadStatus(uint32_t rms, uint32_t startThreshold, uint32_t continueThreshold) {
  const uint32_t now = millis();
  if (now - lastVadStatusAt < kVadStatusIntervalMs || recordingTransport == Transport::None) return;
  lastVadStatusAt = now;
  uint8_t payload[12] = {
    static_cast<uint8_t>((handsFreeMonitoring ? 1 : 0) | (recording ? 2 : 0)),
    0, 0, 0, 0, 0, 0, 0, 0,
    vadSpeechChunks,
    vadSilenceChunks,
    vadCalibrationChunksRemaining,
  };
  writeUint16(payload, 1, rms);
  writeUint16(payload, 3, vadNoiseFloorRms);
  writeUint16(payload, 5, startThreshold);
  writeUint16(payload, 7, continueThreshold);
  writeFrameTo(recordingTransport, FrameType::CaptureStatus, payload, sizeof(payload));
}

void appendVadPreroll(const uint8_t* bytes, size_t length) {
  if (!bytes || !length) return;
  if (length >= sizeof(vadPreroll)) {
    memcpy(vadPreroll, bytes + length - sizeof(vadPreroll), sizeof(vadPreroll));
    vadPrerollLength = sizeof(vadPreroll);
    return;
  }
  if (vadPrerollLength + length > sizeof(vadPreroll)) {
    const size_t discard = vadPrerollLength + length - sizeof(vadPreroll);
    memmove(vadPreroll, vadPreroll + discard, vadPrerollLength - discard);
    vadPrerollLength -= discard;
  }
  memcpy(vadPreroll + vadPrerollLength, bytes, length);
  vadPrerollLength += length;
}

void streamMicrophone() {
  if ((!recording && !handsFreeMonitoring) || audioMode != AudioMode::Microphone) return;
  size_t bytesRead = 0;
  if (i2s_read(kI2sPort, microphoneBuffer, sizeof(microphoneBuffer), &bytesRead, pdMS_TO_TICKS(8)) == ESP_OK
      && bytesRead > 0) {
    if (!handsFreeMonitoring) {
      writeFrameTo(recordingTransport, FrameType::PcmChunk, microphoneBuffer, static_cast<uint16_t>(bytesRead));
      return;
    }
    const uint32_t rms = microphoneRms(microphoneBuffer, bytesRead);
    uint32_t startThreshold = voiceStartThresholdRms();
    uint32_t continueThreshold = voiceContinueThresholdRms();
    if (!recording) {
      appendVadPreroll(microphoneBuffer, bytesRead);
      if (vadCalibrationChunksRemaining) {
        vadCalibrationMinimumRms = min(vadCalibrationMinimumRms, rms);
        --vadCalibrationChunksRemaining;
        if (!vadCalibrationChunksRemaining && vadCalibrationMinimumRms != UINT32_MAX) {
          vadNoiseFloorRms = max<uint32_t>(30, min<uint32_t>(2000, vadCalibrationMinimumRms));
          startThreshold = voiceStartThresholdRms();
          continueThreshold = voiceContinueThresholdRms();
        }
        sendVadStatus(rms, startThreshold, continueThreshold);
        return;
      }
      if (rms >= startThreshold) {
        if (vadSpeechChunks < 255) ++vadSpeechChunks;
      } else {
        vadSpeechChunks = 0;
        vadNoiseFloorRms = (vadNoiseFloorRms * 15 + rms) / 16;
      }
      sendVadStatus(rms, startThreshold, continueThreshold);
      if (vadSpeechChunks >= kVadSpeechStartChunks) beginHandsFreeUtterance();
      return;
    }
    writeFrameTo(recordingTransport, FrameType::PcmChunk, microphoneBuffer, static_cast<uint16_t>(bytesRead));
    if (handsFreeButtonForced) return;
    if (rms >= continueThreshold) vadSilenceChunks = 0;
    else if (vadSilenceChunks < 255) ++vadSilenceChunks;
    sendVadStatus(rms, startThreshold, continueThreshold);
    if (vadSilenceChunks >= kVadSilenceEndChunks
        || millis() - handsFreeUtteranceStartedAt >= kHandsFreeMaximumUtteranceMs) {
      stopRecording();
    }
  }
}

void ensureHandsFreeMonitoring() {
  if (!handsFreeEnabled || handsFreeMonitoring || recording || playing || !transportConnected(activeTransport)) return;
  if (deviceState != DeviceState::Idle && deviceState != DeviceState::Listening) return;
  if (handsFreeResumeAt && static_cast<int32_t>(millis() - handsFreeResumeAt) < 0) return;
  handsFreeResumeAt = 0;
  startHandsFreeMonitoring();
}

String hardwareDeviceId() {
  WiFi.mode(WIFI_STA);
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  mac.toLowerCase();
  return String("atom-echo-") + mac;
}

}  // namespace

void setup() {
  M5.begin(false, false, true);
  Serial.begin(kSerialBaud);
  pinMode(kButtonPin, INPUT_PULLUP);
  deviceId = hardwareDeviceId();
  WiFi.setSleep(false);
  loadWifiConfiguration();
  setState(DeviceState::Connecting);
  delay(120);
  sendHello(Transport::Usb);
  if (wifiSsid.length() && pairingToken.length() == 64) beginWifiAttempt();
}

void loop() {
  consumeHostFrames(Transport::Usb, Serial, usbReceive);
  if (!usbHostConnected && millis() - lastUsbHelloAt >= 1000) sendHello(Transport::Usb);
  updateWifi();
  updateButton();
  ensureHandsFreeMonitoring();
  streamMicrophone();
  consumeHostFrames(Transport::Usb, Serial, usbReceive);
  if (wifiClient.connected()) consumeHostFrames(Transport::Wifi, wifiClient, wifiReceive);
  delay(1);
}
