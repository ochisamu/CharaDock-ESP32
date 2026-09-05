// SPDX-License-Identifier: Apache-2.0
#include "charadock/monochrome_asset.hpp"

#include <algorithm>
#include <cstring>

#include "charadock/protocol_v2.hpp"

namespace charadock::rlcd {
namespace {

constexpr size_t kMetadataPrefixBytes = 15;

uint16_t readU16(const uint8_t *bytes) {
  return static_cast<uint16_t>(bytes[0]) |
         static_cast<uint16_t>(bytes[1]) << 8;
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
  bytes[3] = static_cast<uint8_t>(value >> 24);
}

bool isSafeIdentifier(const uint8_t *bytes, size_t length, bool frameName) {
  if (!bytes)
    return length == 0;
  for (size_t index = 0; index < length; ++index) {
    const uint8_t value = bytes[index];
    const bool alphanumeric = (value >= 'a' && value <= 'z') ||
                              (value >= 'A' && value <= 'Z') ||
                              (value >= '0' && value <= '9');
    const bool punctuation = value == '-' || value == '_' || value == '.' ||
                             (!frameName && value == ':');
    if (!alphanumeric && !punctuation)
      return false;
  }
  return true;
}

size_t boundedLength(const char *value, size_t capacity) {
  size_t length = 0;
  while (length < capacity && value[length] != '\0')
    ++length;
  return length;
}

bool metadataValid(const MonochromeAssetMetadata &metadata) {
  const size_t revisionLength = boundedLength(
      metadata.revision.data(), metadata.revision.size());
  const size_t frameLength = boundedLength(
      metadata.frameName.data(), metadata.frameName.size());
  if (metadata.format != MonochromeFormat::Raw1Msb || metadata.width == 0 ||
      metadata.height == 0 ||
      metadata.width > MonochromeAssetStore::kMaximumWidth ||
      metadata.height > MonochromeAssetStore::kMaximumHeight ||
      revisionLength == 0 ||
      revisionLength > MonochromeAssetMetadata::kMaximumRevisionBytes ||
      frameLength > MonochromeAssetMetadata::kMaximumFrameNameBytes ||
      !isSafeIdentifier(
          reinterpret_cast<const uint8_t *>(metadata.revision.data()),
          revisionLength, false) ||
      !isSafeIdentifier(
          reinterpret_cast<const uint8_t *>(metadata.frameName.data()),
          frameLength, true))
    return false;
  const uint32_t expected =
      ((static_cast<uint32_t>(metadata.width) + 7u) / 8u) * metadata.height;
  return metadata.byteCount == expected &&
         expected <= MonochromeAssetStore::kMaximumBytes;
}

} // namespace

bool MonochromeAssetStore::attach(uint8_t *firstSlot, uint8_t *secondSlot,
                                  size_t slotBytes) {
  if (!firstSlot || !secondSlot || firstSlot == secondSlot ||
      slotBytes < kMaximumBytes)
    return false;
  slots_[0] = firstSlot;
  slots_[1] = secondSlot;
  slotBytes_ = slotBytes;
  activeSlot_ = 0;
  stagingSlot_ = 1;
  available_ = false;
  cancelTransfer();
  return true;
}

MonochromeAssetResult MonochromeAssetStore::beginTransfer(
    const MonochromeAssetMetadata &metadata) {
  if (!slots_[0] || !slots_[1])
    return MonochromeAssetResult::StorageUnavailable;
  if (!metadataValid(metadata))
    return metadata.byteCount > slotBytes_ ? MonochromeAssetResult::TooLarge
                                           : MonochromeAssetResult::InvalidMetadata;
  if (metadata.byteCount > slotBytes_)
    return MonochromeAssetResult::TooLarge;
  stagingSlot_ = available_ ? static_cast<uint8_t>(activeSlot_ ^ 1u) : 0;
  stagedMetadata_ = metadata;
  receivedBytes_ = 0;
  transferActive_ = true;
  return MonochromeAssetResult::Ok;
}

MonochromeAssetResult MonochromeAssetStore::writeChunk(
    uint32_t offset, const uint8_t *bytes, size_t length) {
  if (!transferActive_)
    return MonochromeAssetResult::TransferNotActive;
  if (offset != receivedBytes_)
    return MonochromeAssetResult::UnexpectedOffset;
  if ((!bytes && length != 0) || length == 0 ||
      receivedBytes_ + length > stagedMetadata_.byteCount ||
      receivedBytes_ + length > slotBytes_)
    return MonochromeAssetResult::TooLarge;
  std::memcpy(slots_[stagingSlot_] + receivedBytes_, bytes, length);
  receivedBytes_ += length;
  return MonochromeAssetResult::Ok;
}

MonochromeAssetResult MonochromeAssetStore::finishTransfer() {
  if (!transferActive_)
    return MonochromeAssetResult::TransferNotActive;
  if (receivedBytes_ != stagedMetadata_.byteCount) {
    cancelTransfer();
    return MonochromeAssetResult::Incomplete;
  }
  const uint32_t checksum =
      protocol::crc32(slots_[stagingSlot_], receivedBytes_) ^ 0xffffffffu;
  if (checksum != stagedMetadata_.checksum) {
    cancelTransfer();
    return MonochromeAssetResult::ChecksumMismatch;
  }
  activeSlot_ = stagingSlot_;
  activeMetadata_ = stagedMetadata_;
  available_ = true;
  cancelTransfer();
  return MonochromeAssetResult::Ok;
}

void MonochromeAssetStore::cancelTransfer() {
  transferActive_ = false;
  receivedBytes_ = 0;
  stagedMetadata_ = MonochromeAssetMetadata{};
}

void MonochromeAssetStore::invalidate(const uint8_t *revision, size_t length) {
  if (revision && !matchesRevision(revision, length))
    return;
  available_ = false;
  activeMetadata_ = MonochromeAssetMetadata{};
  cancelTransfer();
}

bool MonochromeAssetStore::available() const { return available_; }
bool MonochromeAssetStore::transferActive() const { return transferActive_; }
const uint8_t *MonochromeAssetStore::pixels() const {
  return available_ ? slots_[activeSlot_] : nullptr;
}
const MonochromeAssetMetadata *MonochromeAssetStore::metadata() const {
  return available_ ? &activeMetadata_ : nullptr;
}
size_t MonochromeAssetStore::receivedBytes() const { return receivedBytes_; }

bool MonochromeAssetStore::matchesRevision(const uint8_t *revision,
                                           size_t length) const {
  if (!available_ || !revision || length == 0 ||
      length > MonochromeAssetMetadata::kMaximumRevisionBytes)
    return false;
  const size_t activeLength = boundedLength(activeMetadata_.revision.data(),
                                            activeMetadata_.revision.size());
  return activeLength == length &&
         std::memcmp(activeMetadata_.revision.data(), revision, length) == 0;
}

bool decodeMonochromeAssetMetadata(const std::vector<uint8_t> &payload,
                                   MonochromeAssetMetadata &metadata) {
  if (payload.size() < kMetadataPrefixBytes)
    return false;
  const size_t revisionLength = payload[13];
  const size_t frameLength = payload[14];
  if (revisionLength == 0 ||
      revisionLength > MonochromeAssetMetadata::kMaximumRevisionBytes ||
      frameLength > MonochromeAssetMetadata::kMaximumFrameNameBytes ||
      payload.size() != kMetadataPrefixBytes + revisionLength + frameLength)
    return false;
  const uint8_t *revision = payload.data() + kMetadataPrefixBytes;
  const uint8_t *frame = revision + revisionLength;
  if (!isSafeIdentifier(revision, revisionLength, false) ||
      !isSafeIdentifier(frame, frameLength, true))
    return false;
  MonochromeAssetMetadata decoded;
  decoded.format = static_cast<MonochromeFormat>(payload[0]);
  decoded.width = readU16(payload.data() + 1);
  decoded.height = readU16(payload.data() + 3);
  decoded.byteCount = readU32(payload.data() + 5);
  decoded.checksum = readU32(payload.data() + 9);
  std::memcpy(decoded.revision.data(), revision, revisionLength);
  std::memcpy(decoded.frameName.data(), frame, frameLength);
  if (!metadataValid(decoded))
    return false;
  metadata = decoded;
  return true;
}

std::vector<uint8_t> encodeMonochromeAssetMetadata(
    const MonochromeAssetMetadata &metadata) {
  if (!metadataValid(metadata))
    return {};
  const size_t revisionLength = boundedLength(metadata.revision.data(),
                                              metadata.revision.size());
  const size_t frameLength = boundedLength(metadata.frameName.data(),
                                           metadata.frameName.size());
  std::vector<uint8_t> payload(kMetadataPrefixBytes + revisionLength +
                               frameLength, 0);
  payload[0] = static_cast<uint8_t>(metadata.format);
  writeU16(payload.data() + 1, metadata.width);
  writeU16(payload.data() + 3, metadata.height);
  writeU32(payload.data() + 5, metadata.byteCount);
  writeU32(payload.data() + 9, metadata.checksum);
  payload[13] = static_cast<uint8_t>(revisionLength);
  payload[14] = static_cast<uint8_t>(frameLength);
  std::memcpy(payload.data() + kMetadataPrefixBytes,
              metadata.revision.data(), revisionLength);
  std::memcpy(payload.data() + kMetadataPrefixBytes + revisionLength,
              metadata.frameName.data(), frameLength);
  return payload;
}

const char *monochromeAssetResultName(MonochromeAssetResult result) {
  switch (result) {
  case MonochromeAssetResult::Ok:
    return "ok";
  case MonochromeAssetResult::StorageUnavailable:
    return "storage-unavailable";
  case MonochromeAssetResult::InvalidMetadata:
    return "invalid-metadata";
  case MonochromeAssetResult::TransferNotActive:
    return "transfer-not-active";
  case MonochromeAssetResult::UnexpectedOffset:
    return "unexpected-offset";
  case MonochromeAssetResult::TooLarge:
    return "too-large";
  case MonochromeAssetResult::Incomplete:
    return "incomplete";
  case MonochromeAssetResult::ChecksumMismatch:
    return "checksum-mismatch";
  }
  return "unknown";
}

} // namespace charadock::rlcd
