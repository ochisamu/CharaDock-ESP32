// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace charadock::protocol {

constexpr uint8_t kVersion = 2;
constexpr size_t kHeaderBytes = 12;
constexpr size_t kMaximumPayloadBytes = 4096;
constexpr size_t kMaximumBufferedBytes =
    (kHeaderBytes + kMaximumPayloadBytes) * 3;

enum class FrameType : uint8_t {
  DeviceHello = 0x01,
  HostHello = 0x02,
  AuthChallenge = 0x03,
  DeviceAuth = 0x04,
  Capabilities = 0x05,
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
  TimeSync = 0x34,
  CharacterChanged = 0x40,
  PresentationConfig = 0x41,
  Expression = 0x42,
  MouthLevel = 0x43,
  Motion = 0x44,
  AssetMeta = 0x50,
  AssetChunk = 0x51,
  AssetEnd = 0x52,
  AssetInvalidate = 0x53,
  DisplayMode = 0x54,
  DisplayScene = 0x55,
  DisplayText = 0x56,
  SensorReport = 0x57,
  InputEvent = 0x58,
  DisplayCommit = 0x59,
  Ack = 0x7e,
  Error = 0x7f,
};

struct Frame {
  FrameType type = FrameType::Error;
  uint16_t sequence = 0;
  std::vector<uint8_t> payload;
};

uint32_t crc32(const uint8_t *bytes, size_t length,
               uint32_t initial = 0xffffffffu);
uint32_t frameCrc(const uint8_t *header, const uint8_t *payload,
                  size_t payloadLength);
std::vector<uint8_t> encodeFrame(const Frame &frame);

class Decoder {
public:
  std::vector<Frame> push(const uint8_t *bytes, size_t length);
  void reset();
  size_t bufferedBytes() const;
  size_t rejectedFrames() const;

private:
  void trimBuffer();

  std::vector<uint8_t> buffer_;
  size_t rejectedFrames_ = 0;
};

} // namespace charadock::protocol
