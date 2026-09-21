// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <Arduino.h>
#include "charadock/protocol_v2.hpp"
namespace charadock {
// Framed Stream transport used by authenticated Wi-Fi. Poll work is bounded
// so display, capture, and speaker tasks cannot be starved by a busy peer.
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
  std::vector<protocol::Frame> poll() {
    uint8_t bytes[512];
    size_t count = 0;
    while (count < sizeof(bytes) && stream_.available())
      bytes[count++] = static_cast<uint8_t>(stream_.read());
    return decoder_.push(bytes, count);
  }
private:
  Stream &stream_;
  protocol::Decoder decoder_;
};
}
