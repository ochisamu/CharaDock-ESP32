// SPDX-License-Identifier: Apache-2.0
#include "charadock/usb_transport.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>

#ifndef CHARADOCK_RLCD_FIRMWARE_VERSION
#define CHARADOCK_RLCD_FIRMWARE_VERSION "development"
#endif

namespace charadock::rlcd {
namespace {

void writeU16(uint8_t *bytes, uint16_t value) {
  bytes[0] = static_cast<uint8_t>(value & 0xff);
  bytes[1] = static_cast<uint8_t>(value >> 8);
}
void writeU32(uint8_t *bytes, uint32_t value) {
  bytes[0] = static_cast<uint8_t>(value & 0xff);
  bytes[1] = static_cast<uint8_t>((value >> 8) & 0xff);
  bytes[2] = static_cast<uint8_t>((value >> 16) & 0xff);
  bytes[3] = static_cast<uint8_t>(value >> 24);
}

} // namespace

UsbTransport::UsbTransport(Stream &stream, const char *transportName)
    : stream_(stream), transportName_(transportName ? transportName : "usb") {}

std::vector<protocol::Frame> UsbTransport::poll() {
  std::vector<protocol::Frame> frames;
  uint8_t bytes[512];
  while (stream_.available() > 0) {
    const size_t wanted =
        std::min<size_t>(sizeof(bytes), static_cast<size_t>(stream_.available()));
    const size_t received = stream_.readBytes(bytes, wanted);
    if (!received)
      break;
    auto decoded = decoder_.push(bytes, received);
    frames.insert(frames.end(), std::make_move_iterator(decoded.begin()),
                  std::make_move_iterator(decoded.end()));
  }
  return frames;
}

void UsbTransport::reset() { decoder_.reset(); }

bool UsbTransport::send(protocol::FrameType type, uint16_t sequence,
                        const std::vector<uint8_t> &payload) {
  return send(type, sequence, payload.data(), payload.size());
}

bool UsbTransport::send(protocol::FrameType type, uint16_t sequence,
                        const uint8_t *payload, size_t length) {
  if ((!payload && length) || length > protocol::kMaximumPayloadBytes)
    return false;
  protocol::Frame frame;
  frame.type = type;
  frame.sequence = sequence;
  if (payload && length)
    frame.payload.assign(payload, payload + length);
  const auto encoded = protocol::encodeFrame(frame);
  return !encoded.empty() && stream_.write(encoded.data(), encoded.size()) ==
                                 encoded.size();
}

bool UsbTransport::respond(const protocol::Frame &request,
                           const FrameApplyOutcome &outcome) {
  const uint8_t payload[] = {
      static_cast<uint8_t>(request.type), static_cast<uint8_t>(outcome.result),
      static_cast<uint8_t>(outcome.assetResult), outcome.audioResult};
  const bool accepted =
      outcome.result == FrameApplyResult::Applied ||
      outcome.result == FrameApplyResult::AssetCompleted ||
      outcome.result == FrameApplyResult::AssetCacheHit ||
      outcome.result == FrameApplyResult::Ignored;
  return send(accepted ? protocol::FrameType::Ack : protocol::FrameType::Error,
              request.sequence, payload, sizeof(payload));
}

bool UsbTransport::sendCapabilities(uint16_t sequence, const char *deviceId) {
  char payload[1400] = {};
  const int length = std::snprintf(
      payload, sizeof(payload),
      "{\"protocol\":2,\"deviceId\":\"%s\","
      "\"board\":\"waveshare-esp32-s3-rlcd-4.2\","
      "\"firmware\":\"%s\",\"capabilities\":{"
      "\"display\":{\"width\":400,\"height\":300,"
      "\"bitsPerPixel\":1,\"orientation\":\"landscape\","
      "\"localFonts\":[\"shinonome-12\",\"shinonome-16\"],"
      "\"bitmap\":[\"raw1-msb\"]},"
      "\"audio\":{\"capture\":true,\"playback\":true,"
      "\"format\":\"pcm-s16le-mono\",\"sampleRates\":[16000],"
      "\"duplex\":\"half\",\"prebufferMs\":256},"
      "\"network\":{\"wifi\":true,\"provisioning\":\"usb-only\","
      "\"authentication\":\"mutual-hmac-sha256\"},"
      "\"input\":[\"key\",\"boot-long\"],"
      "\"sensors\":[\"temperature\",\"humidity\",\"rtc\","
      "\"battery\"],\"storage\":[\"flash\",\"sd-optional\"]}}",
      deviceId ? deviceId : "", CHARADOCK_RLCD_FIRMWARE_VERSION);
  if (length <= 0 || static_cast<size_t>(length) >= sizeof(payload))
    return false;
  return send(protocol::FrameType::Capabilities, sequence,
              reinterpret_cast<const uint8_t *>(payload), length);
}

bool UsbTransport::sendDeviceHello(uint16_t sequence, const char *deviceId) {
  char payload[320] = {};
  const int length = std::snprintf(
      payload, sizeof(payload),
      "{\"board\":\"waveshare-esp32-s3-rlcd-4.2\","
      "\"firmware\":\"%s\",\"deviceId\":\"%s\","
      "\"transport\":\"%s\"}",
      CHARADOCK_RLCD_FIRMWARE_VERSION, deviceId ? deviceId : "",
      transportName_);
  if (length <= 0 || static_cast<size_t>(length) >= sizeof(payload))
    return false;
  return send(protocol::FrameType::DeviceHello, sequence,
              reinterpret_cast<const uint8_t *>(payload), length);
}

bool UsbTransport::sendInput(uint16_t sequence, ButtonId button,
                             InputEventCode event, uint32_t durationMs) {
  uint8_t payload[7] = {1, static_cast<uint8_t>(button),
                        static_cast<uint8_t>(event), 0, 0, 0, 0};
  writeU32(payload + 3, durationMs);
  return send(protocol::FrameType::InputEvent, sequence, payload,
              sizeof(payload));
}

bool UsbTransport::sendSensors(uint16_t sequence,
                               const SensorSnapshot &sensors) {
  uint8_t payload[18] = {};
  payload[0] = 1;
  payload[1] = (sensors.shtc3Available ? 0x01 : 0) |
               (sensors.rtcAvailable ? 0x02 : 0) |
               (sensors.batteryAvailable ? 0x04 : 0) |
               (sensors.es8311Available ? 0x08 : 0) |
               (sensors.es7210Available ? 0x10 : 0);
  writeU16(payload + 2,
           static_cast<uint16_t>(static_cast<int16_t>(
               sensors.temperatureC * 100.0f)));
  writeU16(payload + 4,
           static_cast<uint16_t>(sensors.humidityPercent * 100.0f));
  writeU16(payload + 6,
           static_cast<uint16_t>(sensors.batteryVolts * 1000.0f));
  payload[8] = sensors.batteryPercent;
  writeU16(payload + 9, sensors.dateTime.year);
  payload[11] = sensors.dateTime.month;
  payload[12] = sensors.dateTime.day;
  payload[13] = sensors.dateTime.hour;
  payload[14] = sensors.dateTime.minute;
  payload[15] = sensors.dateTime.second;
  writeU16(payload + 16, 0);
  return send(protocol::FrameType::SensorReport, sequence, payload,
              sizeof(payload));
}

} // namespace charadock::rlcd
