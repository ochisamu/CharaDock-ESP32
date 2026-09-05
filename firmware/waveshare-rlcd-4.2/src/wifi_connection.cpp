// SPDX-License-Identifier: Apache-2.0
#include "charadock/wifi_connection.hpp"

#include <ArduinoJson.h>
#include <Preferences.h>
#include <mbedtls/md.h>

#include <cstdio>
#include <cstring>

namespace charadock::rlcd {
namespace {

constexpr uint16_t kDiscoveryPort = 41721;
constexpr uint16_t kDiscoveryLocalPort = 41723;
constexpr uint16_t kDefaultHostPort = 41722;
constexpr uint32_t kAuthenticationTimeoutMs = 7000;
constexpr char kDiscoveryPrefix[] = "CHARADOCK_DEVICE_DISCOVER_V2 ";
constexpr char kHostPrefix[] = "CHARADOCK_DEVICE_HOST_V2 ";
constexpr char kHostProofDomain[] = "CHARADOCK_HOST_V2:";
constexpr char kBoardId[] = "waveshare-esp32-s3-rlcd-4.2";

uint8_t hexNibble(char value) {
  return value >= '0' && value <= '9'
             ? static_cast<uint8_t>(value - '0')
             : static_cast<uint8_t>(value - 'a' + 10);
}

} // namespace

WifiConnection::WifiConnection() : transport_(client_, "wifi") {}

void WifiConnection::begin(const char *deviceId) {
  deviceId_ = deviceId ? deviceId : "";
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  loadConfiguration();
  if (configured())
    beginWifiAttempt(millis());
}

void WifiConnection::loadConfiguration() {
  Preferences storage;
  storage.begin("charadock", true);
  ssid_ = storage.getString("ssid", "");
  password_ = storage.getString("password", "");
  pairingToken_ = storage.getString("pairing", "");
  storage.end();
  if (!validPairingToken(pairingToken_.c_str()))
    pairingToken_ = "";
}

bool WifiConnection::validPairingToken(const char *token) const {
  if (!token || std::strlen(token) != 64)
    return false;
  for (size_t index = 0; index < 64; ++index) {
    const char value = token[index];
    if (!((value >= '0' && value <= '9') ||
          (value >= 'a' && value <= 'f')))
      return false;
  }
  return true;
}

bool WifiConnection::configured() const {
  return !ssid_.isEmpty() && validPairingToken(pairingToken_.c_str());
}

bool WifiConnection::networkConnected() const {
  return WiFi.status() == WL_CONNECTED;
}

bool WifiConnection::socketConnected() { return client_.connected(); }

bool WifiConnection::hostAuthenticated() {
  return hostAuthenticated_ && socketConnected();
}

bool WifiConnection::markHostAuthenticated(const uint8_t *proof,
                                           size_t length) {
  if (!authenticationProofSent_ || !hostProofReady_ || !socketConnected() ||
      !proof || length != sizeof(expectedHostProof_))
    return false;
  uint8_t difference = 0;
  for (size_t index = 0; index < sizeof(expectedHostProof_); ++index)
    difference |= static_cast<uint8_t>(proof[index] ^ expectedHostProof_[index]);
  if (difference != 0)
    return false;
  hostAuthenticated_ = true;
  hostProofReady_ = false;
  socketOpenedAtMs_ = 0;
  std::memset(expectedHostProof_, 0, sizeof(expectedHostProof_));
  return true;
}

void WifiConnection::resetHost() {
  client_.stop();
  transport_.reset();
  hostAuthenticated_ = false;
  authenticationProofSent_ = false;
  hostProofReady_ = false;
  std::memset(expectedHostProof_, 0, sizeof(expectedHostProof_));
  socketOpened_ = false;
  socketOpenedAtMs_ = 0;
  lastHelloAtMs_ = 0;
}

bool WifiConnection::takeSocketOpened() {
  const bool value = socketOpened_;
  socketOpened_ = false;
  return value;
}

bool WifiConnection::shouldSendHello(uint32_t nowMs) {
  if (!socketConnected() || hostAuthenticated_ ||
      nowMs - lastHelloAtMs_ < 1000)
    return false;
  lastHelloAtMs_ = nowMs;
  return true;
}

void WifiConnection::beginWifiAttempt(uint32_t nowMs) {
  if (!configured())
    return;
  resetHost();
  WiFi.disconnect();
  WiFi.setSleep(false);
  WiFi.begin(ssid_.c_str(), password_.c_str());
  lastWifiAttemptAtMs_ = nowMs;
}

void WifiConnection::discoverHost(uint32_t nowMs) {
  if (!udpStarted_)
    udpStarted_ = discovery_.begin(kDiscoveryLocalPort) == 1;
  if (!udpStarted_)
    return;

  if (nowMs - lastDiscoveryAtMs_ >= 1500) {
    const String request = String(kDiscoveryPrefix) + deviceId_ + " " +
                           kBoardId;
    discovery_.beginPacket(IPAddress(255, 255, 255, 255), kDiscoveryPort);
    discovery_.write(reinterpret_cast<const uint8_t *>(request.c_str()),
                     request.length());
    discovery_.endPacket();
    lastDiscoveryAtMs_ = nowMs;
  }

  const int packetBytes = discovery_.parsePacket();
  if (packetBytes <= 0)
    return;
  char response[128] = {};
  const int length = discovery_.read(response, sizeof(response) - 1);
  if (length <= 0 ||
      std::strncmp(response, kHostPrefix, std::strlen(kHostPrefix)) != 0)
    return;
  char advertisedAddress[64] = {};
  int port = kDefaultHostPort;
  if (std::sscanf(response + std::strlen(kHostPrefix), "%63s %d",
                  advertisedAddress, &port) < 1 ||
      port < 1024 || port > 65535)
    return;

  // The UDP source is authoritative on a private LAN. This avoids trusting
  // a stale or unusable adapter address embedded in the reply.
  if (!client_.connect(discovery_.remoteIP(), static_cast<uint16_t>(port),
                       1200))
    return;
  client_.setNoDelay(true);
  transport_.reset();
  hostAuthenticated_ = false;
  authenticationProofSent_ = false;
  socketOpened_ = true;
  socketOpenedAtMs_ = nowMs;
  lastHelloAtMs_ = 0;
}

void WifiConnection::update(uint32_t nowMs) {
  if (!configured())
    return;
  if (!networkConnected()) {
    if (socketConnected() || hostAuthenticated_)
      resetHost();
    if (!lastWifiAttemptAtMs_ || nowMs - lastWifiAttemptAtMs_ >= 15000)
      beginWifiAttempt(nowMs);
    return;
  }
  if (socketConnected() && !hostAuthenticated_ && socketOpenedAtMs_ &&
      nowMs - socketOpenedAtMs_ >= kAuthenticationTimeoutMs) {
    resetHost();
    return;
  }
  if (!socketConnected()) {
    if (hostAuthenticated_)
      resetHost();
    discoverHost(nowMs);
  }
}

std::vector<protocol::Frame> WifiConnection::poll() {
  if (!socketConnected())
    return {};
  return transport_.poll();
}

UsbTransport &WifiConnection::transport() { return transport_; }

bool WifiConnection::provision(const uint8_t *payload, size_t length,
                               const char *&error) {
  error = "";
  JsonDocument document;
  const DeserializationError parsed = deserializeJson(document, payload, length);
  const char *ssid = document["ssid"] | "";
  const char *password = document["password"] | "";
  const char *token = document["token"] | "";
  if (parsed || !ssid[0] || std::strlen(ssid) > 32 ||
      std::strlen(password) > 64 || !validPairingToken(token)) {
    error = "invalid Wi-Fi configuration";
    return false;
  }

  Preferences storage;
  if (!storage.begin("charadock", false)) {
    error = "configuration storage unavailable";
    return false;
  }
  const bool saved = storage.putString("ssid", ssid) == std::strlen(ssid) &&
                     storage.putString("password", password) ==
                         std::strlen(password) &&
                     storage.putString("pairing", token) ==
                         std::strlen(token);
  storage.end();
  if (!saved) {
    error = "configuration storage failed";
    return false;
  }
  ssid_ = ssid;
  password_ = password;
  pairingToken_ = token;
  beginWifiAttempt(millis());
  return true;
}

bool WifiConnection::createAuthenticationProof(const uint8_t *challenge,
                                               size_t length,
                                               uint8_t output[32]) {
  if (!challenge || length != 32 || !output ||
      !validPairingToken(pairingToken_.c_str()))
    return false;
  uint8_t key[32] = {};
  for (size_t index = 0; index < sizeof(key); ++index) {
    key[index] = static_cast<uint8_t>(
        (hexNibble(pairingToken_[index * 2]) << 4) |
        hexNibble(pairingToken_[index * 2 + 1]));
  }
  const mbedtls_md_info_t *info =
      mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  const bool deviceProofCreated =
      info && mbedtls_md_hmac(info, key, sizeof(key), challenge, length,
                              output) == 0;
  uint8_t hostMaterial[(sizeof(kHostProofDomain) - 1) + 32] = {};
  std::memcpy(hostMaterial, kHostProofDomain, sizeof(kHostProofDomain) - 1);
  std::memcpy(hostMaterial + sizeof(kHostProofDomain) - 1, challenge, length);
  const bool hostProofCreated =
      deviceProofCreated &&
      mbedtls_md_hmac(info, key, sizeof(key), hostMaterial,
                      sizeof(hostMaterial), expectedHostProof_) == 0;
  authenticationProofSent_ = deviceProofCreated && hostProofCreated;
  hostProofReady_ = authenticationProofSent_;
  if (!authenticationProofSent_)
    std::memset(expectedHostProof_, 0, sizeof(expectedHostProof_));
  return authenticationProofSent_;
}

bool WifiConnection::sendStatus(UsbTransport &target, uint16_t sequence,
                                const char *phase, const char *error) {
  JsonDocument document;
  document["configured"] = configured();
  document["connected"] = networkConnected();
  document["deviceId"] = deviceId_;
  document["ssid"] = ssid_;
  document["phase"] = phase ? phase : "idle";
  if (networkConnected())
    document["ip"] = WiFi.localIP().toString();
  if (error && error[0])
    document["error"] = error;
  uint8_t payload[384] = {};
  const size_t payloadLength =
      serializeJson(document, payload, sizeof(payload));
  return payloadLength > 0 &&
         target.send(protocol::FrameType::WifiStatus, sequence, payload,
                     payloadLength);
}

} // namespace charadock::rlcd
