// SPDX-License-Identifier: Apache-2.0
#include "charadock/protocol_v2.hpp"

#include <algorithm>

namespace charadock::protocol {
namespace {

uint16_t readU16(const uint8_t *bytes) {
  return static_cast<uint16_t>(bytes[0]) | static_cast<uint16_t>(bytes[1]) << 8;
}

uint32_t readU32(const uint8_t *bytes) {
  return static_cast<uint32_t>(bytes[0]) |
         static_cast<uint32_t>(bytes[1]) << 8 |
         static_cast<uint32_t>(bytes[2]) << 16 |
         static_cast<uint32_t>(bytes[3]) << 24;
}

void writeU16(uint8_t *bytes, uint16_t value) {
  bytes[0] = static_cast<uint8_t>(value & 0xff);
  bytes[1] = static_cast<uint8_t>(value >> 8);
}

void writeU32(uint8_t *bytes, uint32_t value) {
  bytes[0] = static_cast<uint8_t>(value & 0xff);
  bytes[1] = static_cast<uint8_t>((value >> 8) & 0xff);
  bytes[2] = static_cast<uint8_t>((value >> 16) & 0xff);
  bytes[3] = static_cast<uint8_t>((value >> 24) & 0xff);
}

} // namespace

uint32_t crc32(const uint8_t *bytes, size_t length, uint32_t initial) {
  uint32_t crc = initial;
  for (size_t index = 0; index < length; ++index) {
    crc ^= bytes[index];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ ((crc & 1u) ? 0xedb88320u : 0u);
    }
  }
  return crc;
}

uint32_t frameCrc(const uint8_t *header, const uint8_t *payload,
                  size_t payloadLength) {
  uint32_t crc = crc32(header + 2, 6);
  if (payload && payloadLength)
    crc = crc32(payload, payloadLength, crc);
  return crc ^ 0xffffffffu;
}

std::vector<uint8_t> encodeFrame(const Frame &frame) {
  if (frame.payload.size() > kMaximumPayloadBytes)
    return {};
  std::vector<uint8_t> bytes(kHeaderBytes + frame.payload.size(), 0);
  bytes[0] = 'C';
  bytes[1] = 'D';
  bytes[2] = kVersion;
  bytes[3] = static_cast<uint8_t>(frame.type);
  writeU16(bytes.data() + 4, frame.sequence);
  writeU16(bytes.data() + 6, static_cast<uint16_t>(frame.payload.size()));
  if (!frame.payload.empty()) {
    std::copy(frame.payload.begin(), frame.payload.end(),
              bytes.begin() + kHeaderBytes);
  }
  writeU32(bytes.data() + 8, frameCrc(bytes.data(), bytes.data() + kHeaderBytes,
                                      frame.payload.size()));
  return bytes;
}

std::vector<Frame> Decoder::push(const uint8_t *bytes, size_t length) {
  if (bytes && length)
    buffer_.insert(buffer_.end(), bytes, bytes + length);
  trimBuffer();
  std::vector<Frame> frames;
  while (buffer_.size() >= kHeaderBytes) {
    auto magic = std::search(buffer_.begin(), buffer_.end(), "CD", "CD" + 2);
    if (magic != buffer_.begin()) {
      if (magic == buffer_.end()) {
        const bool keepTrailingC = !buffer_.empty() && buffer_.back() == 'C';
        buffer_.assign(keepTrailingC ? 1 : 0, static_cast<uint8_t>('C'));
        break;
      }
      buffer_.erase(buffer_.begin(), magic);
      if (buffer_.size() < kHeaderBytes)
        break;
    }
    if (buffer_[2] != kVersion) {
      ++rejectedFrames_;
      buffer_.erase(buffer_.begin());
      continue;
    }
    const size_t payloadLength = readU16(buffer_.data() + 6);
    if (payloadLength > kMaximumPayloadBytes) {
      ++rejectedFrames_;
      buffer_.erase(buffer_.begin());
      continue;
    }
    const size_t frameLength = kHeaderBytes + payloadLength;
    if (buffer_.size() < frameLength)
      break;
    const uint32_t expected = readU32(buffer_.data() + 8);
    const uint32_t actual =
        frameCrc(buffer_.data(), buffer_.data() + kHeaderBytes, payloadLength);
    if (expected != actual) {
      ++rejectedFrames_;
      buffer_.erase(buffer_.begin());
      continue;
    }
    Frame frame;
    frame.type = static_cast<FrameType>(buffer_[3]);
    frame.sequence = readU16(buffer_.data() + 4);
    frame.payload.assign(buffer_.begin() + kHeaderBytes,
                         buffer_.begin() + frameLength);
    frames.push_back(std::move(frame));
    buffer_.erase(buffer_.begin(), buffer_.begin() + frameLength);
  }
  return frames;
}

void Decoder::reset() {
  buffer_.clear();
  rejectedFrames_ = 0;
}

size_t Decoder::bufferedBytes() const { return buffer_.size(); }

size_t Decoder::rejectedFrames() const { return rejectedFrames_; }

void Decoder::trimBuffer() {
  if (buffer_.size() <= kMaximumBufferedBytes)
    return;
  const size_t overflow = buffer_.size() - kMaximumBufferedBytes;
  buffer_.erase(buffer_.begin(), buffer_.begin() + overflow);
  ++rejectedFrames_;
}

} // namespace charadock::protocol
