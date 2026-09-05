// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiUdp.h>

#include "charadock/protocol_v2.hpp"
#include "charadock/usb_transport.hpp"

namespace charadock::rlcd {

// Device-neutral Protocol v2 LAN transport. Wi-Fi credentials and the
// per-device HMAC secret can only be provisioned by a frame received on USB;
// this class never accepts provisioning over its network stream.
class WifiConnection {
public:
  WifiConnection();

  void begin(const char *deviceId);
  void update(uint32_t nowMs);
  std::vector<protocol::Frame> poll();
  UsbTransport &transport();

  bool configured() const;
  bool networkConnected() const;
  bool socketConnected();
  bool hostAuthenticated();
  bool markHostAuthenticated(const uint8_t *proof, size_t length);
  void resetHost();
  bool takeSocketOpened();
  bool shouldSendHello(uint32_t nowMs);

  bool provision(const uint8_t *payload, size_t length, const char *&error);
  bool createAuthenticationProof(const uint8_t *challenge, size_t length,
                                 uint8_t output[32]);
  bool sendStatus(UsbTransport &target, uint16_t sequence, const char *phase,
                  const char *error = "");

private:
  void loadConfiguration();
  void beginWifiAttempt(uint32_t nowMs);
  void discoverHost(uint32_t nowMs);
  bool validPairingToken(const char *token) const;

  WiFiClient client_;
  WiFiUDP discovery_;
  UsbTransport transport_;
  String deviceId_;
  String ssid_;
  String password_;
  String pairingToken_;
  bool udpStarted_ = false;
  bool hostAuthenticated_ = false;
  bool authenticationProofSent_ = false;
  bool hostProofReady_ = false;
  uint8_t expectedHostProof_[32] = {};
  bool socketOpened_ = false;
  uint32_t socketOpenedAtMs_ = 0;
  uint32_t lastWifiAttemptAtMs_ = 0;
  uint32_t lastDiscoveryAtMs_ = 0;
  uint32_t lastHelloAtMs_ = 0;
};

} // namespace charadock::rlcd
