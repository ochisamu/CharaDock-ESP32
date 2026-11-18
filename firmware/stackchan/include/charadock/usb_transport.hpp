// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <Arduino.h>
#include "charadock/protocol_v2.hpp"
namespace charadock {
// Framed transport shared by USB and authenticated Wi-Fi.
class UsbTransport {
public:
  UsbTransport(Stream &stream, const char *) : stream_(stream) {}
  void reset() { decoder_.reset(); }
  bool send(protocol::FrameType type, uint16_t sequence,
            const uint8_t *bytes = nullptr, size_t length = 0) {
    protocol::Frame frame{type, sequence, {}};
    if (length) frame.payload.assign(bytes, bytes + length);
    auto encoded = protocol::encodeFrame(frame);
    return !encoded.empty() && stream_.write(encoded.data(), encoded.size()) == encoded.size();
  }
  std::vector<protocol::Frame> receive(const uint8_t *bytes, size_t length) {
    return decoder_.push(bytes, length);
  }
private:
  Stream &stream_;
  protocol::Decoder decoder_;
};
}
