// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <vector>

#include <Arduino.h>

#include "charadock/board.hpp"
#include "charadock/frame_dispatcher.hpp"
#include "charadock/input.hpp"
#include "charadock/protocol_v2.hpp"

namespace charadock::rlcd {

enum class InputEventCode : uint8_t {
  PhysicalPress = 0,
  ToggleOverview = 1,
  PttStart = 2,
  PttEnd = 3,
  Interrupt = 4,
  Diagnostic = 5,
  Reconnect = 6,
};

class UsbTransport {
public:
  explicit UsbTransport(Stream &stream, const char *transportName = "usb");
  std::vector<protocol::Frame> poll();
  void reset();
  bool send(protocol::FrameType type, uint16_t sequence,
            const std::vector<uint8_t> &payload = {});
  bool send(protocol::FrameType type, uint16_t sequence,
            const uint8_t *payload, size_t length);
  bool respond(const protocol::Frame &request,
               const FrameApplyOutcome &outcome);
  bool sendCapabilities(uint16_t sequence, const char *deviceId);
  bool sendDeviceHello(uint16_t sequence, const char *deviceId);
  bool sendInput(uint16_t sequence, ButtonId button, InputEventCode event,
                 uint32_t durationMs);
  bool sendSensors(uint16_t sequence, const SensorSnapshot &sensors);

private:
  Stream &stream_;
  const char *transportName_;
  protocol::Decoder decoder_;
};

} // namespace charadock::rlcd
